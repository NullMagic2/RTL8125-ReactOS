# Module: rtl8125.package.staging
# Purpose: Copy complete corresponding source and an identified operational candidate.
# Created: 2026-10-24
# Architecture: Invoked only after actual PE/INF checks by the opt-in package target.
cmake_minimum_required(VERSION 3.16)
foreach(required PACKAGE_SOURCE PACKAGE_BINARY PACKAGE_STAGE PACKAGE_COMPILER
                 PACKAGE_SDK_SOURCE PACKAGE_SDK_BUILD)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing explicit packaging input ${required}")
    endif()
endforeach()
if(NOT EXISTS "${PACKAGE_BINARY}" OR NOT EXISTS "${PACKAGE_SOURCE}/LICENSE")
    message(FATAL_ERROR "Missing built candidate or corresponding source")
endif()
foreach(input rtl8125.inf INSTALL.txt BUILD.txt FIRMWARE.txt REACTOS_COMPATIBILITY.txt notices)
    if(NOT EXISTS "${PACKAGE_SOURCE}/packaging/${input}")
        message(FATAL_ERROR "Incomplete operational package input: ${input}")
    endif()
endforeach()
foreach(input 0001-ndis-halt-context.patch 0002-ndis-callback-rundown.patch 0003-ndis-device-power.patch)
    if(NOT EXISTS "${PACKAGE_SOURCE}/patches/reactos/${input}")
        message(FATAL_ERROR "Required ordered ReactOS compatibility patch missing: ${input}")
    endif()
endforeach()
file(SHA256 "${PACKAGE_BINARY}" driver_hash)
string(SUBSTRING "${driver_hash}" 0 16 short_hash)
set(stage "${PACKAGE_STAGE}/rtl8125-i386-${short_hash}")
if(EXISTS "${stage}")
    message(FATAL_ERROR "Candidate already exists; preserve it and select a new owned build directory: ${stage}")
endif()
file(MAKE_DIRECTORY "${stage}" "${stage}/source")
file(COPY "${PACKAGE_BINARY}" DESTINATION "${stage}")
get_filename_component(binary_name "${PACKAGE_BINARY}" NAME)
if(NOT binary_name STREQUAL "rtl8125.sys")
    file(RENAME "${stage}/${binary_name}" "${stage}/rtl8125.sys")
endif()
foreach(input rtl8125.inf INSTALL.txt BUILD.txt FIRMWARE.txt REACTOS_COMPATIBILITY.txt notices)
    file(COPY "${PACKAGE_SOURCE}/packaging/${input}" DESTINATION "${stage}")
endforeach()
foreach(input LICENSE THIRD_PARTY.txt)
    file(COPY "${PACKAGE_SOURCE}/${input}" DESTINATION "${stage}")
endforeach()
foreach(input src cmake tests packaging third_party docs evidence)
    file(COPY "${PACKAGE_SOURCE}/${input}" DESTINATION "${stage}/source"
        PATTERN "STYLE_GUIDE.md" EXCLUDE PATTERN "AGENTS.md" EXCLUDE
        PATTERN "agents.md" EXCLUDE PATTERN "agents.txt" EXCLUDE)
endforeach()
file(COPY "${PACKAGE_SOURCE}/validation" DESTINATION "${stage}/source"
    PATTERN "*.sys" EXCLUDE PATTERN "*.tar.gz" EXCLUDE PATTERN "*.iso" EXCLUDE)
file(MAKE_DIRECTORY "${stage}/source/patches" "${stage}/patches")
file(COPY "${PACKAGE_SOURCE}/patches/reactos" DESTINATION "${stage}/source/patches")
file(COPY "${PACKAGE_SOURCE}/patches/reactos" DESTINATION "${stage}/patches")
foreach(input CMakeLists.txt LICENSE THIRD_PARTY.txt ARCHITECTURE.md roadmap.json)
    file(COPY "${PACKAGE_SOURCE}/${input}" DESTINATION "${stage}/source"
        PATTERN "STYLE_GUIDE.md" EXCLUDE PATTERN "AGENTS.md" EXCLUDE
        PATTERN "agents.md" EXCLUDE PATTERN "agents.txt" EXCLUDE)
endforeach()
if(EXISTS "${PACKAGE_SOURCE}/README.md")
    file(COPY "${PACKAGE_SOURCE}/README.md" DESTINATION "${stage}/source")
endif()
# Keep publication exclusions consistent even for nested documentation snapshots.
file(GLOB_RECURSE public_source_files LIST_DIRECTORIES false "${stage}/source/*")
foreach(input IN LISTS public_source_files)
    get_filename_component(public_name "${input}" NAME)
    string(TOLOWER "${public_name}" public_name)
    if(public_name STREQUAL "style_guide.md" OR public_name STREQUAL "agents.md" OR public_name STREQUAL "agents.txt")
        file(REMOVE "${input}")
    endif()
endforeach()
execute_process(COMMAND "${PACKAGE_COMPILER}" --version OUTPUT_VARIABLE compiler_version
    RESULT_VARIABLE compiler_status OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT compiler_status EQUAL 0)
    message(FATAL_ERROR "Cannot identify the actual native compiler")
endif()
file(WRITE "${stage}/BUILD_INFO.txt"
    "Module: rtl8125.package.build_identity\nCreated: 2026-10-24\n"
    "Variant: operational experimental, RTL8125_OPERATIONAL=1\nArchitecture: i386\n"
    "NDIS: 5.1\nReactOS wrapper patch acknowledged: yes; runtime installation must be verified\n"
    "Compiler: ${compiler_version}\nSDK source: ${PACKAGE_SDK_SOURCE}\nSDK build: ${PACKAGE_SDK_BUILD}\n"
    "Driver SHA256: ${driver_hash}\nFirmware bundled: no\n"
    "Guest install/packet/physical validation: pending\nMSVC verification: pending\n")
foreach(input "${PACKAGE_SDK_SOURCE}/sdk/include/ddk/ndis.h"
              "${PACKAGE_SDK_BUILD}/sdk/include/ddk/ntddk.h"
              "${PACKAGE_SDK_BUILD}/drivers/network/ndis/CMakeFiles/libndis.dir/libndis.a"
              "${PACKAGE_SDK_BUILD}/ntoskrnl/CMakeFiles/libntoskrnl.dir/libntoskrnl.a"
              "${PACKAGE_SDK_BUILD}/hal/halx86/CMakeFiles/libhal.dir/libhal.a")
    if(NOT EXISTS "${input}")
        message(FATAL_ERROR "SDK provenance input disappeared: ${input}")
    endif()
    file(SHA256 "${input}" checksum)
    file(APPEND "${stage}/BUILD_INFO.txt" "SDK SHA256 ${checksum}  ${input}\n")
endforeach()
foreach(input 0001-ndis-halt-context.patch 0002-ndis-callback-rundown.patch 0003-ndis-device-power.patch)
    file(SHA256 "${PACKAGE_SOURCE}/patches/reactos/${input}" checksum)
    file(APPEND "${stage}/BUILD_INFO.txt" "Required patch SHA256 ${checksum}  patches/reactos/${input}\n")
endforeach()
file(GLOB_RECURSE source_files RELATIVE "${stage}/source" LIST_DIRECTORIES false "${stage}/source/*")
list(SORT source_files)
file(WRITE "${stage}/source/SHA256SUMS.txt" "")
foreach(input IN LISTS source_files)
    file(SHA256 "${stage}/source/${input}" checksum)
    file(APPEND "${stage}/source/SHA256SUMS.txt" "${checksum}  ${input}\n")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${stage}/source.tar.gz" --format=gnutar source
    WORKING_DIRECTORY "${stage}" RESULT_VARIABLE archive_status)
if(NOT archive_status EQUAL 0)
    message(FATAL_ERROR "Corresponding source archive failed; staging retained")
endif()
file(GLOB_RECURSE package_files RELATIVE "${stage}" LIST_DIRECTORIES false "${stage}/*")
list(FILTER package_files EXCLUDE REGEX "^source/")
list(SORT package_files)
file(WRITE "${stage}/SHA256SUMS.txt" "")
foreach(input IN LISTS package_files)
    file(SHA256 "${stage}/${input}" checksum)
    file(APPEND "${stage}/SHA256SUMS.txt" "${checksum}  ${input}\n")
endforeach()
message(STATUS "Candidate staged without installation: ${stage}")
