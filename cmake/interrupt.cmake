# Module: rtl8125.build.interrupt
# Purpose: Genuine-SDK interrupt registration and tracked scheduler ABI checks.
# Created: 2026-10-24
# Architecture: Reuses the actual production IRQ object; device/OS imports mocked.
if(RTL8125_NATIVE_DRIVER)
    set(interrupt_contract "${CMAKE_CURRENT_BINARY_DIR}/interrupt-native/interrupt_contract_tests.exe")
    add_custom_command(OUTPUT "${interrupt_contract}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/interrupt-native"
        COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
            -D_CRT_NON_CONFORMING_SWPRINTFS -static-libgcc -static-libstdc++
            "${PROJECT_SOURCE_DIR}/tests/interrupt_native_tests.cpp"
            "${PROJECT_SOURCE_DIR}/tests/recovery_import_stubs.cpp" ${native_interrupt_objects}
            "${contract_miniport}"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_oid.cpp.obj"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_core_chip.cpp.obj"
            -o "${interrupt_contract}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/interrupt_native_tests.cpp"
            "${PROJECT_SOURCE_DIR}/tests/recovery_import_stubs.cpp" "${contract_miniport}"
            ${native_interrupt_objects} ${native_objects}
        VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_interrupt_contract_tests DEPENDS "${interrupt_contract}")
endif()
