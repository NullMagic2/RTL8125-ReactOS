# Module: rtl8125.build.native
# Purpose: Opt-in i386 driver link against a user's existing ReactOS SDK build.
# Created: 2026-10-24
# Architecture: Read-only external SDK; all outputs stay in the owned build tree.
option(RTL8125_NATIVE_DRIVER "Build a ReactOS i386 native driver (read-only probe by default)" OFF)
option(RTL8125_OPERATIONAL "Build the experimental operational miniport instead of the read-only probe" OFF)
option(RTL8125_REACTOS_WRAPPER_PATCHED "Acknowledge the required ReactOS NDIS compatibility patch for operational use" OFF)
option(RTL8125_GUEST_FIXTURE "Build isolated guest-only kernel lifecycle fixtures" OFF)
set(RTL8125_NATIVE_ARCH "i386" CACHE STRING "Native architecture; only i386 is implemented")
set_property(CACHE RTL8125_NATIVE_ARCH PROPERTY STRINGS i386)
set(RTL8125_REACTOS_SOURCE "" CACHE PATH "Matching ReactOS source directory")
set(RTL8125_REACTOS_BUILD "" CACHE PATH "Existing matching i386 ReactOS build directory")
set(RTL8125_NATIVE_CXX "i686-w64-mingw32-g++" CACHE FILEPATH "GCC 13 i386 MinGW C++ compiler")

if(RTL8125_OPERATIONAL AND NOT RTL8125_NATIVE_DRIVER)
    message(FATAL_ERROR "RTL8125_OPERATIONAL requires RTL8125_NATIVE_DRIVER=ON; portable host builds do not produce a driver")
endif()

if(RTL8125_NATIVE_DRIVER)
    if(NOT RTL8125_NATIVE_ARCH STREQUAL "i386")
        message(FATAL_ERROR "Only the i386 native target is implemented; AMD64 requires separate source/ABI qualification")
    endif()
    if(RTL8125_OPERATIONAL AND NOT RTL8125_REACTOS_WRAPPER_PATCHED)
        message(FATAL_ERROR "Operational use requires the explicit ReactOS NDIS patch acknowledgment; see packaging/REACTOS_COMPATIBILITY.txt")
    endif()
    execute_process(COMMAND "${RTL8125_NATIVE_CXX}" -dumpmachine
        OUTPUT_VARIABLE native_machine OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE native_machine_status)
    if(NOT native_machine_status EQUAL 0 OR NOT native_machine STREQUAL "i686-w64-mingw32")
        message(FATAL_ERROR "Standalone native build requires i686-w64-mingw32 GCC; MSVC uses packaging/reactos inside a matching ReactOS MSVC build")
    endif()
    foreach(required
            "${RTL8125_REACTOS_SOURCE}/sdk/include/ddk/ndis.h"
            "${RTL8125_REACTOS_BUILD}/sdk/include/ddk/ntddk.h"
            "${RTL8125_REACTOS_BUILD}/drivers/network/ndis/CMakeFiles/libndis.dir/libndis.a"
            "${RTL8125_REACTOS_BUILD}/ntoskrnl/CMakeFiles/libntoskrnl.dir/libntoskrnl.a"
            "${RTL8125_REACTOS_BUILD}/hal/halx86/CMakeFiles/libhal.dir/libhal.a")
        if(NOT EXISTS "${required}")
            message(FATAL_ERROR "Native SDK input missing: ${required}")
        endif()
    endforeach()

    set(native_flags -std=c++17 -O2 -DNDIS51_MINIPORT -DNDIS_MINIPORT_DRIVER
        -DNDIS_LEGACY_MINIPORT=1 -D__REACTOS__ -D_X86_ -D_M_IX86
        -DWINVER=0x502 -D_WIN32_WINNT=0x502 -DNTDDI_VERSION=0x05020100
        -fms-extensions -fno-exceptions -fno-rtti
        -ffreestanding -fno-stack-protector -fno-threadsafe-statics
        -fno-use-cxa-atexit -Wall -Wextra -Werror -Wno-multichar
        -Wno-unknown-pragmas)
    set(native_include_flags)
    foreach(sdk_path sdk/include/c++ sdk/include/crt sdk/include/vcruntime
                     sdk/include sdk/include/ddk sdk/include/ndk
                     sdk/include/psdk sdk/include/reactos)
        list(APPEND native_include_flags -isystem "${RTL8125_REACTOS_SOURCE}/${sdk_path}")
    endforeach()
    list(APPEND native_include_flags -isystem "${RTL8125_REACTOS_SOURCE}/sdk/lib/pseh/include")
    foreach(sdk_path sdk/include sdk/include/ddk sdk/include/psdk sdk/include/reactos)
        list(APPEND native_include_flags -isystem "${RTL8125_REACTOS_BUILD}/${sdk_path}")
    endforeach()

    include("${CMAKE_CURRENT_LIST_DIR}/driver_sources.cmake")
    set(native_packet_objects)
    set(native_hardware_objects)
    set(native_dma_hardware_objects)
    set(native_recovery_objects)
    set(native_interrupt_objects)
    file(GLOB native_headers CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/core/*.hpp" "${PROJECT_SOURCE_DIR}/src/reactos/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/hardware/*.hpp")
    set(native_objects)
    foreach(source IN LISTS native_sources)
        string(REPLACE "/" "_" object_name "${source}")
        set(object_directory "${CMAKE_CURRENT_BINARY_DIR}/native")
        set(source_flags ${native_flags})
        if(source STREQUAL "src/reactos/miniport.cpp" AND RTL8125_OPERATIONAL)
            list(APPEND source_flags -DRTL8125_OPERATIONAL=1)
            set(object_directory "${CMAKE_CURRENT_BINARY_DIR}/native/operational")
        endif()
        set(object "${object_directory}/${object_name}.obj")
        add_custom_command(OUTPUT "${object}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${object_directory}"
            COMMAND "${RTL8125_NATIVE_CXX}" ${source_flags} ${native_include_flags}
                -c "${PROJECT_SOURCE_DIR}/${source}" -o "${object}"
            DEPENDS "${PROJECT_SOURCE_DIR}/${source}" ${native_headers}
            VERBATIM COMMAND_EXPAND_LISTS)
        list(APPEND native_objects "${object}")
        if(source IN_LIST native_hardware_sources)
            list(APPEND native_hardware_objects "${object}")
            if(NOT source STREQUAL "src/reactos/hardware_init.cpp")
                list(APPEND native_dma_hardware_objects "${object}")
            endif()
        endif()
        if(source IN_LIST native_recovery_sources)
            list(APPEND native_recovery_objects "${object}")
        endif()
        if(source STREQUAL "src/reactos/interrupt.cpp")
            list(APPEND native_interrupt_objects "${object}")
        endif()
        if(source IN_LIST native_packet_sources)
            list(APPEND native_packet_objects "${object}")
        endif()
    endforeach()

    set(native_imports
        "${RTL8125_REACTOS_BUILD}/drivers/network/ndis/CMakeFiles/libndis.dir/libndis.a"
        "${RTL8125_REACTOS_BUILD}/ntoskrnl/CMakeFiles/libntoskrnl.dir/libntoskrnl.a"
        "${RTL8125_REACTOS_BUILD}/hal/halx86/CMakeFiles/libhal.dir/libhal.a")
    set(native_driver "${CMAKE_CURRENT_BINARY_DIR}/native/rtl8125.sys")
    if(RTL8125_OPERATIONAL)
        set(native_driver "${CMAKE_CURRENT_BINARY_DIR}/native/operational/rtl8125.sys")
    endif()
    add_custom_command(OUTPUT "${native_driver}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/native/operational"
        COMMAND "${RTL8125_NATIVE_CXX}" -nostdlib -shared
            -Wl,--entry,_DriverEntry@8 -Wl,--subsystem,native -Wl,--image-base,0x10000
            -Wl,--major-os-version,5 -Wl,--minor-os-version,2
            -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2
            -Wl,--disable-auto-image-base -Wl,--disable-runtime-pseudo-reloc
            -Wl,--disable-auto-import -Wl,--disable-stdcall-fixup -Wl,--exclude-all-symbols
            -o "${native_driver}" ${native_objects} ${native_imports}
        DEPENDS ${native_objects} ${native_imports}
        VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_driver DEPENDS "${native_driver}")

    # This executable injects user-mode import pointers into the genuine driver
    # objects. It verifies callback contracts, never kernel/guest compatibility.
    set(contract_miniport "${CMAKE_CURRENT_BINARY_DIR}/native/contract_miniport.obj")
    add_custom_command(OUTPUT "${contract_miniport}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/native"
        COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
            -c "${PROJECT_SOURCE_DIR}/src/reactos/miniport.cpp" -o "${contract_miniport}"
        DEPENDS "${PROJECT_SOURCE_DIR}/src/reactos/miniport.cpp" ${native_headers}
        VERBATIM COMMAND_EXPAND_LISTS)
    set(contract_executable "${CMAKE_CURRENT_BINARY_DIR}/native/miniport_contract_tests.exe")
    add_custom_command(OUTPUT "${contract_executable}"
        COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
            -D_CRT_NON_CONFORMING_SWPRINTFS -static-libgcc -static-libstdc++
            "${PROJECT_SOURCE_DIR}/tests/miniport_contract_tests.cpp"
            "${PROJECT_SOURCE_DIR}/tests/datapath_import_stubs.cpp"
            "${PROJECT_SOURCE_DIR}/tests/interrupt_import_stubs.cpp"
            "${PROJECT_SOURCE_DIR}/tests/recovery_import_stubs.cpp"
            ${native_interrupt_objects} ${native_packet_objects} ${native_dma_hardware_objects}
            "${contract_miniport}"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_oid.cpp.obj"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_chip.cpp.obj"
            -o "${contract_executable}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/miniport_contract_tests.cpp"
            "${PROJECT_SOURCE_DIR}/tests/datapath_import_stubs.cpp"
            "${PROJECT_SOURCE_DIR}/tests/interrupt_import_stubs.cpp"
            "${PROJECT_SOURCE_DIR}/tests/recovery_import_stubs.cpp" "${contract_miniport}" ${native_objects} VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_contract_tests DEPENDS "${contract_executable}")

    set(resource_contract_executable "${CMAKE_CURRENT_BINARY_DIR}/native/resources_contract_tests.exe")
    add_custom_command(OUTPUT "${resource_contract_executable}"
        COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
            -D_CRT_NON_CONFORMING_SWPRINTFS -static-libgcc -static-libstdc++
            "${PROJECT_SOURCE_DIR}/tests/resources_native_tests.cpp"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_reactos_resources.cpp.obj"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_resources.cpp.obj"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_chip.cpp.obj"
            -o "${resource_contract_executable}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/resources_native_tests.cpp"
            ${native_objects} VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_resource_contract_tests DEPENDS "${resource_contract_executable}")

    if(RTL8125_GUEST_FIXTURE)
        set(guest_production_object "${CMAKE_CURRENT_BINARY_DIR}/native/guest_production_entry.obj")
        set(guest_fixture_object "${CMAKE_CURRENT_BINARY_DIR}/native/guest_kernel.obj")
        set(guest_driver "${CMAKE_CURRENT_BINARY_DIR}/native/rtl8125_m01_guest.sys")
        set(guest_runner "${CMAKE_CURRENT_BINARY_DIR}/native/miniport_guest_runner.exe")
        add_custom_command(OUTPUT "${guest_production_object}" "${guest_fixture_object}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/native"
            COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
                -DDriverEntry=rtl8125_production_entry
                -c "${PROJECT_SOURCE_DIR}/src/reactos/miniport.cpp" -o "${guest_production_object}"
            COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
                -c "${PROJECT_SOURCE_DIR}/tests/miniport_guest_kernel.cpp" -o "${guest_fixture_object}"
            DEPENDS "${PROJECT_SOURCE_DIR}/src/reactos/miniport.cpp"
                "${PROJECT_SOURCE_DIR}/tests/miniport_guest_kernel.cpp" ${native_headers}
            VERBATIM COMMAND_EXPAND_LISTS)
        add_custom_command(OUTPUT "${guest_driver}"
            COMMAND "${RTL8125_NATIVE_CXX}" -nostdlib -shared
                -Wl,--entry,_DriverEntry@8 -Wl,--subsystem,native -Wl,--image-base,0x10000
                -Wl,--major-os-version,5 -Wl,--minor-os-version,2
                -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2
                -Wl,--disable-auto-image-base -Wl,--disable-runtime-pseudo-reloc
                -Wl,--disable-auto-import -Wl,--disable-stdcall-fixup -Wl,--exclude-all-symbols
                "${guest_production_object}" "${guest_fixture_object}" ${native_packet_objects}
                ${native_interrupt_objects} ${native_hardware_objects} ${native_recovery_objects}
                "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_chip.cpp.obj"
                "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_oid.cpp.obj"
                "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_resources.cpp.obj"
                "${CMAKE_CURRENT_BINARY_DIR}/native/src_reactos_resources.cpp.obj"
                ${native_imports} -o "${guest_driver}"
            DEPENDS "${guest_production_object}" "${guest_fixture_object}" ${native_objects}
                ${native_imports} VERBATIM COMMAND_EXPAND_LISTS)
        add_custom_command(OUTPUT "${guest_runner}"
            COMMAND "${RTL8125_NATIVE_CXX}" -std=c++17 -O2 -Wall -Wextra -Werror
                -fno-exceptions -fno-rtti -static-libgcc -static-libstdc++
                "${PROJECT_SOURCE_DIR}/tests/miniport_guest_runner.cpp"
                -ladvapi32 -o "${guest_runner}"
            DEPENDS "${PROJECT_SOURCE_DIR}/tests/miniport_guest_runner.cpp" VERBATIM)
        add_custom_target(rtl8125_guest_fixture DEPENDS "${guest_driver}" "${guest_runner}")
    endif()
endif()
