# Module: rtl8125.build
# Purpose: Build the portable core, boundary checks and opt-in native miniport.
# Created: 2026-10-24
# Architecture: Native SDK compilation/linking lives in cmake/native.cmake.
cmake_minimum_required(VERSION 3.16)
project(rtl8125_reactos LANGUAGES CXX)

option(RTL8125_SANITIZERS "Enable native ASan/UBSan verification" OFF)
option(RTL8125_TESTS "Build portable host tests" ON)

add_library(rtl8125_core STATIC
    src/core/chip.cpp src/core/descriptor.cpp src/core/receive.cpp src/core/oid.cpp src/core/resources.cpp
    src/core/probe_log.cpp)
target_include_directories(rtl8125_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/core")

function(rtl8125_cpp_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /GR- /EHs-c-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
    endif()
    if(RTL8125_SANITIZERS)
        if(MSVC OR CMAKE_CROSSCOMPILING)
            message(FATAL_ERROR "Sanitizer verification requires a native GCC/Clang host")
        endif()
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

rtl8125_cpp_target(rtl8125_core)

add_executable(rtl8125_probe_collect src/tools/probe_collect.cpp)
target_link_libraries(rtl8125_probe_collect PRIVATE rtl8125_core)
rtl8125_cpp_target(rtl8125_probe_collect)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(rtl8125_vfio_check src/tools/vfio_check.cpp)
    target_link_libraries(rtl8125_vfio_check PRIVATE rtl8125_core)
    rtl8125_cpp_target(rtl8125_vfio_check)
endif()

if(RTL8125_TESTS)
    enable_testing()
    add_executable(rtl8125_core_tests tests/core_tests.cpp)
    target_link_libraries(rtl8125_core_tests PRIVATE rtl8125_core)
    rtl8125_cpp_target(rtl8125_core_tests)
    add_test(NAME rtl8125_core_boundaries COMMAND rtl8125_core_tests)

    add_executable(rtl8125_oid_tests tests/oid_tests.cpp)
    target_link_libraries(rtl8125_oid_tests PRIVATE rtl8125_core)
    rtl8125_cpp_target(rtl8125_oid_tests)
    add_test(NAME rtl8125_oid_boundaries COMMAND rtl8125_oid_tests)

    add_executable(rtl8125_resource_tests tests/resources_tests.cpp)
    target_link_libraries(rtl8125_resource_tests PRIVATE rtl8125_core)
    rtl8125_cpp_target(rtl8125_resource_tests)
    add_test(NAME rtl8125_resource_boundaries COMMAND rtl8125_resource_tests)

    add_executable(rtl8125_probe_log_tests tests/probe_log_tests.cpp)
    target_link_libraries(rtl8125_probe_log_tests PRIVATE rtl8125_core)
    rtl8125_cpp_target(rtl8125_probe_log_tests)
    add_test(NAME rtl8125_probe_log_boundaries COMMAND rtl8125_probe_log_tests)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(rtl8125_vfio_check_tests tests/resources_vfio_check_tests.cpp)
        rtl8125_cpp_target(rtl8125_vfio_check_tests)
        add_test(NAME rtl8125_vfio_readiness COMMAND rtl8125_vfio_check_tests)
    endif()
endif()

include(cmake/native.cmake)
include(cmake/hardware.cmake)
include(cmake/datapath.cmake)
include(cmake/interrupt.cmake)

include(cmake/recovery.cmake)
include(cmake/package.cmake)
include(cmake/installer.cmake)
