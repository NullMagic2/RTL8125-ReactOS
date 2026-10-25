# Module: rtl8125.build.package
# Purpose: Validate and stage explicit operational candidate packages without installation.
# Created: 2026-10-24
# Architecture: Native build owns the SYS; this module owns isolated package staging.
add_executable(rtl8125_package_check tests/package_check.cpp)
rtl8125_cpp_target(rtl8125_package_check)

if(RTL8125_TESTS)
    add_executable(rtl8125_package_tests tests/package_tests.cpp)
    rtl8125_cpp_target(rtl8125_package_tests)
    add_test(NAME rtl8125_package_boundaries COMMAND rtl8125_package_tests
        "${PROJECT_SOURCE_DIR}/packaging/rtl8125.inf")
endif()

if(RTL8125_NATIVE_DRIVER AND RTL8125_OPERATIONAL)
    if(NOT RTL8125_REACTOS_WRAPPER_PATCHED)
        message(FATAL_ERROR "Operational package requires explicit patched ReactOS wrapper compatibility")
    endif()
    add_custom_target(rtl8125_candidate_package
        COMMAND "$<TARGET_FILE:rtl8125_package_check>" "${native_driver}"
            "${PROJECT_SOURCE_DIR}/packaging/rtl8125.inf"
        COMMAND "${CMAKE_COMMAND}"
            "-DPACKAGE_SOURCE=${PROJECT_SOURCE_DIR}"
            "-DPACKAGE_BINARY=${native_driver}"
            "-DPACKAGE_STAGE=${CMAKE_CURRENT_BINARY_DIR}/package"
            "-DPACKAGE_COMPILER=${RTL8125_NATIVE_CXX}"
            "-DPACKAGE_SDK_SOURCE=${RTL8125_REACTOS_SOURCE}"
            "-DPACKAGE_SDK_BUILD=${RTL8125_REACTOS_BUILD}"
            -P "${PROJECT_SOURCE_DIR}/packaging/stage.cmake"
        DEPENDS rtl8125_native_driver rtl8125_package_check
        VERBATIM)
endif()
