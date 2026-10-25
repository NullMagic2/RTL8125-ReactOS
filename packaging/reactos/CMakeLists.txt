# Module: rtl8125.build.reactos_in_tree
# Purpose: Build with genuine GCC or MSVC ReactOS toolchain/import targets.
# Created: 2026-10-24
# Architecture: Added explicitly to a separate owned ReactOS source checkout.
if(NOT COMMAND set_module_type OR NOT COMMAND add_importlibs)
    message(FATAL_ERROR "Use this directory inside an actual ReactOS CMake build")
endif()
if(NOT ARCH STREQUAL "i386")
    message(FATAL_ERROR "RTL8125 native architecture is i386; AMD64 is not implemented/qualified")
endif()
if(NOT RTL8125_OPERATIONAL OR NOT RTL8125_REACTOS_WRAPPER_PATCHED)
    message(FATAL_ERROR "Explicit operational build and patched-wrapper acknowledgments are required")
endif()
if(MSVC AND (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR MSVC_VERSION LESS 1930))
    message(FATAL_ERROR "The MSVC configuration targets actual MSVC 2022 (19.3x+) separately from clang-cl")
endif()
get_filename_component(rtl8125_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${rtl8125_source_root}/cmake/driver_sources.cmake")
set(reactos_driver_sources)
foreach(source IN LISTS native_sources)
    list(APPEND reactos_driver_sources "${rtl8125_source_root}/${source}")
endforeach()
add_library(rtl8125 MODULE ${reactos_driver_sources})
set_target_properties(rtl8125 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_definitions(rtl8125 PRIVATE NDIS51_MINIPORT NDIS_MINIPORT_DRIVER
    NDIS_LEGACY_MINIPORT=1 RTL8125_OPERATIONAL=1)
if(MSVC)
    target_compile_options(rtl8125 PRIVATE /W4 /WX /permissive- /GR- /EHs-c- /GS- /Zc:threadSafeInit-)
    target_link_options(rtl8125 PRIVATE /NODEFAULTLIB)
else()
    target_compile_options(rtl8125 PRIVATE -Wall -Wextra -Werror -Wno-multichar
        -Wno-unknown-pragmas -fno-exceptions -fno-rtti -ffreestanding
        -fno-stack-protector -fno-threadsafe-statics -fno-use-cxa-atexit)
endif()
set_module_type(rtl8125 kernelmodedriver)
add_importlibs(rtl8125 ndis ntoskrnl hal)
# Do not automatically add the experimental driver to a boot/install CD or bind
# a device. Package the resulting SYS explicitly with the exact standalone INF.
