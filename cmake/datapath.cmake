# Module: rtl8125.build.datapath
# Purpose: Copied packet values, host boundaries and genuine-SDK datapath checks.
# Created: 2026-10-24
# Architecture: Included after hardware.cmake; no production callback activation.
add_library(rtl8125_packet STATIC src/core/packet.cpp src/core/filter.cpp)
target_link_libraries(rtl8125_packet PUBLIC rtl8125_core)
rtl8125_cpp_target(rtl8125_packet)
if(RTL8125_TESTS)
    add_executable(rtl8125_packet_tests tests/packet_tests.cpp)
    target_link_libraries(rtl8125_packet_tests PRIVATE rtl8125_packet)
    rtl8125_cpp_target(rtl8125_packet_tests)
    add_test(NAME rtl8125_packet_boundaries COMMAND rtl8125_packet_tests)
endif()
if(RTL8125_NATIVE_DRIVER)
    # Link the same objects shipped in the production probe; no duplicate path.
    set(datapath_native_objects ${native_packet_objects})
    add_custom_target(rtl8125_native_datapath_objects DEPENDS ${datapath_native_objects})
    set(datapath_contract "${CMAKE_CURRENT_BINARY_DIR}/datapath-native/datapath_contract_tests.exe")
    add_custom_command(OUTPUT "${datapath_contract}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/datapath-native"
        COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
            -D_CRT_NON_CONFORMING_SWPRINTFS -static-libgcc -static-libstdc++
            "${PROJECT_SOURCE_DIR}/tests/datapath_native_tests.cpp"
            "${PROJECT_SOURCE_DIR}/tests/interrupt_import_stubs.cpp"
            "${PROJECT_SOURCE_DIR}/tests/recovery_import_stubs.cpp"
            ${native_interrupt_objects} ${datapath_native_objects} ${native_dma_hardware_objects}
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_oid.cpp.obj"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_chip.cpp.obj"
            "${contract_miniport}"
            -o "${datapath_contract}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/datapath_native_tests.cpp"
            "${PROJECT_SOURCE_DIR}/tests/interrupt_import_stubs.cpp"
            "${PROJECT_SOURCE_DIR}/tests/recovery_import_stubs.cpp" ${datapath_native_objects}
            ${hardware_native_objects} "${contract_miniport}" ${native_objects}
        VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_datapath_contract_tests DEPENDS "${datapath_contract}")
    set(datapath_link "${CMAKE_CURRENT_BINARY_DIR}/datapath-native/rtl8125-datapath-link.sys")
    add_custom_command(OUTPUT "${datapath_link}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/datapath-native"
        COMMAND "${RTL8125_NATIVE_CXX}" -nostdlib -shared
            -Wl,--entry,_DriverEntry@8 -Wl,--subsystem,native -Wl,--image-base,0x10000
            -Wl,--major-os-version,5 -Wl,--minor-os-version,2
            -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2
            -Wl,--disable-auto-image-base -Wl,--disable-runtime-pseudo-reloc
            -Wl,--disable-auto-import -Wl,--disable-stdcall-fixup -Wl,--exclude-all-symbols
            -o "${datapath_link}" ${native_objects} ${native_imports}
        DEPENDS ${datapath_native_objects} ${native_objects} ${native_imports}
        VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_datapath_link_check DEPENDS "${datapath_link}")
endif()
