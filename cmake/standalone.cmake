# Module: rtl8125.build.standalone
# Purpose: Package the approved driver and dependencies into one offline executable.
# Created: 2026-10-25
# Architecture: Reuse Qemu-Lightning's general package engine; no device policy in it.
option(RTL8125_STANDALONE "Build the standalone driver and prerequisite installer" OFF)
set(RTL8125_PACKAGE_TOOL "" CACHE FILEPATH "Qemu-Lightning lightning-guest-packages executable")
set(RTL8125_PACKAGE_BACKEND "" CACHE FILEPATH "Qemu-Lightning GuestPackageSetup-i386.exe")
set(RTL8125_PACKAGE_CATALOG "" CACHE PATH "Approved catalog root containing catalog.json and verified payloads")
if(RTL8125_STANDALONE)
    foreach(input RTL8125_PACKAGE_TOOL RTL8125_PACKAGE_BACKEND)
        if(NOT EXISTS "${${input}}")
            message(FATAL_ERROR "Standalone packaging needs ${input}")
        endif()
    endforeach()
    if(NOT EXISTS "${RTL8125_PACKAGE_CATALOG}/catalog.json")
        message(FATAL_ERROR "Standalone packaging needs a complete approved RTL8125_PACKAGE_CATALOG")
    endif()
    set(standalone_executable "${CMAKE_CURRENT_BINARY_DIR}/standalone/RTL8125B-Setup.exe")
    add_custom_target(rtl8125_standalone
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/standalone"
        COMMAND "${RTL8125_PACKAGE_TOOL}" prepare-exe "${RTL8125_PACKAGE_CATALOG}"
            "${RTL8125_PACKAGE_BACKEND}" "${standalone_executable}" rtl8125
        BYPRODUCTS "${standalone_executable}"
        COMMENT "Bundle verified RTL8125 driver, firmware and compatibility dependencies"
        VERBATIM)
endif()
