# Module: rtl8125.build.hardware
# Purpose: Portable hardware algorithms, model checks and genuine-SDK DMA objects.
# Created: 2026-10-24
# Architecture: Included after native.cmake; no probe initialization wiring.
add_library(rtl8125_hardware STATIC
    src/hardware/registers.cpp src/hardware/firmware.cpp src/hardware/phy.cpp src/hardware/device.cpp)
target_link_libraries(rtl8125_hardware PUBLIC rtl8125_core)
rtl8125_cpp_target(rtl8125_hardware)

if(RTL8125_TESTS)
    add_executable(rtl8125_hardware_tests tests/hardware_tests.cpp)
    target_link_libraries(rtl8125_hardware_tests PRIVATE rtl8125_hardware)
    rtl8125_cpp_target(rtl8125_hardware_tests)
    add_test(NAME rtl8125_hardware_models COMMAND rtl8125_hardware_tests)
    add_executable(rtl8125_hardware_device_tests tests/hardware_device_tests.cpp)
    target_link_libraries(rtl8125_hardware_device_tests PRIVATE rtl8125_hardware)
    rtl8125_cpp_target(rtl8125_hardware_device_tests)
    add_test(NAME rtl8125_hardware_device_models COMMAND rtl8125_hardware_device_tests)
    add_executable(rtl8125_firmware_check tests/firmware_file_check.cpp)
    target_link_libraries(rtl8125_firmware_check PRIVATE rtl8125_hardware)
    rtl8125_cpp_target(rtl8125_firmware_check)
endif()

if(RTL8125_NATIVE_DRIVER)
    set(hardware_native_objects ${native_hardware_objects})
    add_custom_target(rtl8125_native_hardware_objects DEPENDS ${hardware_native_objects})

    set(dma_contract "${CMAKE_CURRENT_BINARY_DIR}/hardware-native/dma_contract_tests.exe")
    add_custom_command(OUTPUT "${dma_contract}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/hardware-native"
        COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
            -D_CRT_NON_CONFORMING_SWPRINTFS -static-libgcc -static-libstdc++
            "${PROJECT_SOURCE_DIR}/tests/dma_native_tests.cpp" ${hardware_native_objects}
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_reactos_dma.cpp.obj"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_chip.cpp.obj"
            -o "${dma_contract}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/dma_native_tests.cpp" ${hardware_native_objects}
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_reactos_dma.cpp.obj" ${native_objects}
        VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_dma_contract_tests DEPENDS "${dma_contract}")

    set(hardware_contract "${CMAKE_CURRENT_BINARY_DIR}/hardware-native/hardware_contract_tests.exe")
    add_custom_command(OUTPUT "${hardware_contract}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/hardware-native"
        COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
            -DRTL8125_NATIVE_BRIDGE -Wno-unused-function
            -D_CRT_NON_CONFORMING_SWPRINTFS -static-libgcc -static-libstdc++
            "${PROJECT_SOURCE_DIR}/tests/hardware_device_tests.cpp" ${hardware_native_objects}
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_chip.cpp.obj"
            -o "${hardware_contract}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/hardware_device_tests.cpp" ${hardware_native_objects} ${native_objects}
        VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_hardware_contract_tests DEPENDS "${hardware_contract}")

    set(hardware_link "${CMAKE_CURRENT_BINARY_DIR}/hardware-native/rtl8125-hardware-link.sys")
    add_custom_command(OUTPUT "${hardware_link}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/hardware-native"
        COMMAND "${RTL8125_NATIVE_CXX}" -nostdlib -shared
            -Wl,--entry,_DriverEntry@8 -Wl,--subsystem,native -Wl,--image-base,0x10000
            -Wl,--major-os-version,5 -Wl,--minor-os-version,2
            -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2
            -Wl,--disable-auto-image-base -Wl,--disable-runtime-pseudo-reloc
            -Wl,--disable-auto-import -Wl,--disable-stdcall-fixup -Wl,--exclude-all-symbols
            -o "${hardware_link}" ${native_objects} ${native_imports}
        DEPENDS ${native_objects} ${native_imports}
        VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_hardware_link_check DEPENDS "${hardware_link}")
endif()
