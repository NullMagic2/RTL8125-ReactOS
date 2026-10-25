# Module: rtl8125.build.installer
# Purpose: Build portable setup policy checks and an explicit native i386 Win32 wizard.
# Created: 2026-10-24
# Architecture: User-mode GUI is separate from the kernel and requires actual payload hashes.
add_library(rtl8125_installer_model STATIC src/installer/model.cpp)
rtl8125_cpp_target(rtl8125_installer_model)
if(RTL8125_TESTS)
    add_executable(rtl8125_installer_tests tests/installer_tests.cpp)
    target_link_libraries(rtl8125_installer_tests PRIVATE rtl8125_installer_model)
    rtl8125_cpp_target(rtl8125_installer_tests)
    add_test(NAME rtl8125_installer_policy COMMAND rtl8125_installer_tests)
endif()
option(RTL8125_INSTALLER "Build the native ReactOS i386 GUI installer" OFF)
option(RTL8125_INSTALLER_TRUSTED_GUEST "Explicit controlled VM build for audited ReactOS registry ACL stubs" OFF)
set(installer_context_flags)
if(RTL8125_INSTALLER_TRUSTED_GUEST)
    list(APPEND installer_context_flags -DRTL8125_TRUSTED_REACTOS_GUEST=1)
endif()
set(RTL8125_INSTALLER_DRIVER "" CACHE FILEPATH "Actual operational i386 SYS to package with the installer")
set(RTL8125_INSTALLER_NDIS_HASH "d71a8f327a2afda106c56031da84cf17da37f10fb9a32cf0311a9baa43bc7cd3"
    CACHE STRING "Known compatible patched ndis.sys SHA256, from reviewed build evidence")
if(RTL8125_INSTALLER)
    if(NOT RTL8125_NATIVE_ARCH STREQUAL "i386")
        message(FATAL_ERROR "The installer target is i386 only")
    endif()
    if(NOT EXISTS "${RTL8125_INSTALLER_DRIVER}")
        message(FATAL_ERROR "Installer needs an actual operational SYS via RTL8125_INSTALLER_DRIVER")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${RTL8125_INSTALLER_DRIVER}" "${PROJECT_SOURCE_DIR}/packaging/rtl8125.inf")
    string(LENGTH "${RTL8125_INSTALLER_NDIS_HASH}" hash_length)
    if(NOT hash_length EQUAL 64 OR RTL8125_INSTALLER_NDIS_HASH MATCHES "[^0-9a-f]")
        message(FATAL_ERROR "Installer compatibility hash must be an explicit lowercase SHA256")
    endif()
    file(SHA256 "${RTL8125_INSTALLER_DRIVER}" installer_driver_hash)
    file(SHA256 "${PROJECT_SOURCE_DIR}/packaging/rtl8125.inf" installer_inf_hash)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/installer")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/installer/installer_build.hpp"
        "// Generated actual payload identities; no runtime download or OS replacement.\n#pragma once\n"
        "#define RTL8125_SETUP_DRIVER_HASH \"${installer_driver_hash}\"\n"
        "#define RTL8125_SETUP_INF_HASH \"${installer_inf_hash}\"\n"
        "#define RTL8125_SETUP_NDIS_HASH \"${RTL8125_INSTALLER_NDIS_HASH}\"\n")
    set(installer_sources src/installer/main.cpp src/installer/backend.cpp src/installer/model.cpp
        src/hardware/firmware.cpp src/hardware/registers.cpp src/core/chip.cpp)
    if(MSVC)
        if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
            message(FATAL_ERROR "Configure MSVC with Win32; native AMD64 setup is not implemented")
        endif()
        add_executable(rtl8125_setup WIN32 ${installer_sources})
        target_include_directories(rtl8125_setup PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/installer")
        if(RTL8125_INSTALLER_TRUSTED_GUEST)
            target_compile_definitions(rtl8125_setup PRIVATE RTL8125_TRUSTED_REACTOS_GUEST=1)
        endif()
        target_compile_definitions(rtl8125_setup PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0502 WINVER=0x0502)
        rtl8125_cpp_target(rtl8125_setup)
        target_link_libraries(rtl8125_setup PRIVATE setupapi advapi32 user32 gdi32 comctl32 comdlg32 shell32)
        set_target_properties(rtl8125_setup PROPERTIES OUTPUT_NAME "RTL8125-Setup"
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        execute_process(COMMAND "${RTL8125_NATIVE_CXX}" -dumpmachine
            OUTPUT_VARIABLE installer_machine OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE machine_status)
        if(NOT machine_status EQUAL 0 OR NOT installer_machine STREQUAL "i686-w64-mingw32")
            message(FATAL_ERROR "Installer GCC must target i686-w64-mingw32")
        endif()
        set(installer_inputs)
        foreach(input IN LISTS installer_sources)
            list(APPEND installer_inputs "${PROJECT_SOURCE_DIR}/${input}")
        endforeach()
        set(installer_executable "${CMAKE_CURRENT_BINARY_DIR}/installer/RTL8125-Setup.exe")
        add_custom_command(OUTPUT "${installer_executable}"
            COMMAND "${RTL8125_NATIVE_CXX}" ${installer_context_flags} -std=c++17 -O2 -Wall -Wextra -Werror
                -Wno-cast-function-type -fno-exceptions -fno-rtti -static -municode -mwindows
                -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0502 -DWINVER=0x0502
                -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1
                -I "${CMAKE_CURRENT_BINARY_DIR}/installer" ${installer_inputs}
                -lsetupapi -ladvapi32 -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -lshell32
                -o "${installer_executable}"
            DEPENDS ${installer_inputs} "${PROJECT_SOURCE_DIR}/src/installer/model.hpp"
                "${PROJECT_SOURCE_DIR}/src/installer/backend.hpp" "${PROJECT_SOURCE_DIR}/src/installer/package_contract.hpp"
                "${CMAKE_CURRENT_BINARY_DIR}/installer/installer_build.hpp"
            VERBATIM COMMAND_EXPAND_LISTS)
        add_custom_target(rtl8125_setup DEPENDS "${installer_executable}")
        set(preview_test "${CMAKE_CURRENT_BINARY_DIR}/installer/installer_preview_tests.exe")
        add_custom_command(OUTPUT "${preview_test}"
            COMMAND "${RTL8125_NATIVE_CXX}" ${installer_context_flags} -std=c++17 -O2 -Wall -Wextra -Werror -Wno-cast-function-type
                -fno-exceptions -fno-rtti -static -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0502
                -I "${CMAKE_CURRENT_BINARY_DIR}/installer"
                "${PROJECT_SOURCE_DIR}/tests/installer_preview_tests.cpp"
                "${PROJECT_SOURCE_DIR}/src/installer/backend.cpp" "${PROJECT_SOURCE_DIR}/src/installer/model.cpp"
                "${PROJECT_SOURCE_DIR}/src/hardware/firmware.cpp" "${PROJECT_SOURCE_DIR}/src/hardware/registers.cpp"
                "${PROJECT_SOURCE_DIR}/src/core/chip.cpp"
                -lsetupapi -ladvapi32 -o "${preview_test}"
            DEPENDS "${PROJECT_SOURCE_DIR}/tests/installer_preview_tests.cpp" ${installer_inputs}
                "${PROJECT_SOURCE_DIR}/src/installer/model.hpp" "${PROJECT_SOURCE_DIR}/src/installer/backend.hpp"
                "${PROJECT_SOURCE_DIR}/src/installer/package_contract.hpp"
                "${CMAKE_CURRENT_BINARY_DIR}/installer/installer_build.hpp"
            VERBATIM COMMAND_EXPAND_LISTS)
        add_custom_target(rtl8125_installer_preview_tests DEPENDS "${preview_test}")
        set(installer_base "${CMAKE_CURRENT_BINARY_DIR}/installer-base/rtl8125-i386-")
        string(SUBSTRING "${installer_driver_hash}" 0 16 installer_short_hash)
        string(APPEND installer_base "${installer_short_hash}")
        add_custom_target(rtl8125_installer_package
            COMMAND "$<TARGET_FILE:rtl8125_package_check>" "${RTL8125_INSTALLER_DRIVER}"
                "${PROJECT_SOURCE_DIR}/packaging/rtl8125.inf"
            COMMAND "${CMAKE_COMMAND}" "-DPACKAGE_SOURCE=${PROJECT_SOURCE_DIR}"
                "-DPACKAGE_BINARY=${RTL8125_INSTALLER_DRIVER}"
                "-DPACKAGE_STAGE=${CMAKE_CURRENT_BINARY_DIR}/installer-base"
                "-DPACKAGE_COMPILER=${RTL8125_NATIVE_CXX}"
                "-DPACKAGE_SDK_SOURCE=${RTL8125_REACTOS_SOURCE}"
                "-DPACKAGE_SDK_BUILD=${RTL8125_REACTOS_BUILD}"
                -P "${PROJECT_SOURCE_DIR}/packaging/stage.cmake"
            COMMAND "${CMAKE_COMMAND}" "-DINSTALLER_BASE=${installer_base}"
                "-DINSTALLER_EXE=${installer_executable}" "-DINSTALLER_SOURCE=${PROJECT_SOURCE_DIR}"
                "-DINSTALLER_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/installer-release"
                "-DINSTALLER_NDIS_HASH=${RTL8125_INSTALLER_NDIS_HASH}"
                "-DINSTALLER_TRUSTED_GUEST=${RTL8125_INSTALLER_TRUSTED_GUEST}"
                -P "${PROJECT_SOURCE_DIR}/packaging/installer_package.cmake"
            DEPENDS rtl8125_setup rtl8125_package_check
            VERBATIM)
    endif()
endif()
