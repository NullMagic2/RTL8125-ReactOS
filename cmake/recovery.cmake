# Module: rtl8125.build.recovery
# Purpose: Actual M08 native lifecycle with mocked OS/hardware imports.
# Created: 2026-10-24
# Architecture: Genuine SDK ABI and real concurrent system-thread adaptation.
if(RTL8125_NATIVE_DRIVER)
    set(recovery_contract "${CMAKE_CURRENT_BINARY_DIR}/recovery-native/recovery_contract_tests.exe")
    add_custom_command(OUTPUT "${recovery_contract}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/recovery-native"
        COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
            -D_CRT_NON_CONFORMING_SWPRINTFS -static-libgcc -static-libstdc++
            "${PROJECT_SOURCE_DIR}/tests/recovery_native_tests.cpp"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_reactos_recovery.cpp.obj"
            "${CMAKE_CURRENT_BINARY_DIR}/native/src_hardware_registers.cpp.obj"
            -o "${recovery_contract}"
        DEPENDS "${PROJECT_SOURCE_DIR}/tests/recovery_native_tests.cpp" ${native_objects}
        VERBATIM COMMAND_EXPAND_LISTS)
    add_custom_target(rtl8125_native_recovery_contract_tests DEPENDS "${recovery_contract}")

    set(RTL8125_PATCHED_NDIS_SOURCE "" CACHE PATH "Owned NDIS source after the ordered M08 compatibility patches")
    if(RTL8125_PATCHED_NDIS_SOURCE)
        find_program(RTL8125_NATIVE_C i686-w64-mingw32-gcc REQUIRED)
        set(ndis_lifecycle_object "${CMAKE_CURRENT_BINARY_DIR}/recovery-native/ndis_lifecycle.obj")
        set(ndis_power_object "${CMAKE_CURRENT_BINARY_DIR}/recovery-native/ndis_power.obj")
        set(ndis_fixture "${CMAKE_CURRENT_BINARY_DIR}/recovery-native/recovery_ndis_tests.exe")
        set(ndis_flags -std=gnu99 -O2 -DDBG=0 -DNDEBUG= -DNDIS_WRAPPER -DNDIS51
            -DNDIS51_MINIPORT -DNDIS_MINIPORT_DRIVER -DNDIS_LEGACY_DRIVER=1
            -DNDIS_LEGACY_MINIPORT=1 -DNDIS_LEGACY_PROTOCOL=1 -D__REACTOS__
            -D_X86_ -D_M_IX86 -DWINVER=0x502 -D_WIN32_WINNT=0x502 -DNTDDI_VERSION=0x05020100
            -fms-extensions -fno-strict-aliasing -ffunction-sections -fdata-sections
            -Werror=implicit-function-declaration -Werror=incompatible-pointer-types)
        foreach(pair lifecycle power)
            add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/recovery-native/ndis_${pair}.obj"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/recovery-native"
                COMMAND "${RTL8125_NATIVE_C}" ${ndis_flags} ${native_include_flags}
                    "-I${RTL8125_PATCHED_NDIS_SOURCE}/include"
                    -c "${RTL8125_PATCHED_NDIS_SOURCE}/ndis/${pair}.c"
                    -o "${CMAKE_CURRENT_BINARY_DIR}/recovery-native/ndis_${pair}.obj"
                DEPENDS "${RTL8125_PATCHED_NDIS_SOURCE}/ndis/${pair}.c"
                    "${RTL8125_PATCHED_NDIS_SOURCE}/include/miniport.h"
                VERBATIM COMMAND_EXPAND_LISTS)
        endforeach()
        add_custom_command(OUTPUT "${ndis_fixture}"
            COMMAND "${RTL8125_NATIVE_CXX}" ${native_flags} ${native_include_flags}
                -DNDIS_WRAPPER -DNDIS51 -DNDIS_LEGACY_DRIVER=1 -DNDIS_LEGACY_PROTOCOL=1
                "-I${RTL8125_PATCHED_NDIS_SOURCE}/include"
                -D_CRT_NON_CONFORMING_SWPRINTFS -static-libgcc -static-libstdc++
                -Wl,--gc-sections "${PROJECT_SOURCE_DIR}/tests/recovery_ndis_tests.cpp"
                "${ndis_lifecycle_object}" "${ndis_power_object}" -o "${ndis_fixture}"
            DEPENDS "${PROJECT_SOURCE_DIR}/tests/recovery_ndis_tests.cpp"
                "${ndis_lifecycle_object}" "${ndis_power_object}"
            VERBATIM COMMAND_EXPAND_LISTS)
        add_custom_target(rtl8125_patched_ndis_contract_tests DEPENDS "${ndis_fixture}")
    endif()
endif()
