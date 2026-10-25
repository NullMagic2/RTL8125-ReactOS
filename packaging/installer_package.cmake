# Module: rtl8125.package.installer_release
# Purpose: Assemble the reviewed GUI with its exact operational payload and source.
# Created: 2026-10-24
# Architecture: Owned staging only; no system installation or core OS update.
cmake_minimum_required(VERSION 3.16)
foreach(input INSTALLER_BASE INSTALLER_EXE INSTALLER_SOURCE INSTALLER_OUTPUT INSTALLER_NDIS_HASH)
    if(NOT DEFINED ${input} OR "${${input}}" STREQUAL "")
        message(FATAL_ERROR "Missing installer package input ${input}")
    endif()
endforeach()
set(product "RTL8125B-ReactOS-Setup-i386")
set(destination "${INSTALLER_OUTPUT}/${product}")
if(EXISTS "${destination}" OR EXISTS "${INSTALLER_OUTPUT}/${product}.zip")
    message(FATAL_ERROR "Preserve the existing installer candidate; choose a new owned output directory")
endif()
file(MAKE_DIRECTORY "${destination}")
file(GLOB contents "${INSTALLER_BASE}/*")
foreach(input IN LISTS contents)
    get_filename_component(name "${input}" NAME)
    if(NOT name STREQUAL "source")
        file(COPY "${input}" DESTINATION "${destination}")
    endif()
endforeach()
file(COPY "${INSTALLER_EXE}" DESTINATION "${destination}")
file(COPY "${INSTALLER_SOURCE}/packaging/SETUP.txt" DESTINATION "${destination}")
file(SHA256 "${INSTALLER_EXE}" executable_hash)
set(journal_mode "Protected machine journal required")
if(INSTALLER_TRUSTED_GUEST)
    set(journal_mode "CONTROLLED VM ONLY: audited ReactOS ACL stub accepted; journal is not protected; RTL8125_INSTALLER_TRUSTED_GUEST=ON")
endif()
file(WRITE "${destination}/INSTALLER_BUILD_INFO.txt"
    "Module: rtl8125.package.installer_identity\nCreated: 2026-10-24\n"
    "GUI: actual i386 Win32 C++17, static GCC/MinGW runtime\n"
    "Journal mode: ${journal_mode}\n"
    "Executable SHA256: ${executable_hash}\n"
    "Approved patched NDIS SHA256: ${INSTALLER_NDIS_HASH}\n"
    "NDIS binary and Realtek firmware bundled: no\n"
    "Preview UI and denial tests: separate Wine evidence\n"
    "Actual ReactOS installation/removal/reboot/physical validation: pending\n"
    "MSVC build verification: pending\n")
file(GLOB_RECURSE files RELATIVE "${destination}" LIST_DIRECTORIES false "${destination}/*")
list(FILTER files EXCLUDE REGEX "^SHA256SUMS.txt$")
list(SORT files)
file(WRITE "${destination}/SHA256SUMS.txt" "")
foreach(input IN LISTS files)
    file(SHA256 "${destination}/${input}" checksum)
    file(APPEND "${destination}/SHA256SUMS.txt" "${checksum}  ${input}\n")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${INSTALLER_OUTPUT}/${product}.zip" --format=zip "${product}"
    WORKING_DIRECTORY "${INSTALLER_OUTPUT}" RESULT_VARIABLE archive_status)
if(NOT archive_status EQUAL 0)
    message(FATAL_ERROR "Installer ZIP failed; staging was retained")
endif()
message(STATUS "Installer candidate staged without installation: ${INSTALLER_OUTPUT}/${product}.zip")
