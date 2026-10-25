/*
Module: rtl8125.tests.package_check
Purpose: Inspect the actual staged i386 PE and exact Net-class INF without installation.
Created: 2026-10-24
Architecture: Host-only CLI reused by the candidate packaging target.
*/
#include "../src/installer/package_contract.hpp"

#include <cstdio>
#include <fstream>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "Usage: package_check DRIVER.sys rtl8125.inf\n");
        return 2;
    }
    std::ifstream driver(argv[1], std::ios::binary | std::ios::ate);
    std::ifstream inf(argv[2], std::ios::binary | std::ios::ate);
    if (!driver || !inf || driver.tellg() < 0 || driver.tellg() > 16 * 1024 * 1024 ||
        inf.tellg() < 0 || inf.tellg() > 65536) {
        std::fprintf(stderr, "Cannot read bounded driver and INF inputs\n");
        return 2;
    }
    const auto driver_size = static_cast<std::size_t>(driver.tellg());
    const auto inf_size = static_cast<std::size_t>(inf.tellg());
    driver.seekg(0);
    inf.seekg(0);
    std::vector<std::uint8_t> bytes(driver_size);
    std::string text(inf_size, '\0');
    driver.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(driver_size));
    inf.read(text.data(), static_cast<std::streamsize>(inf_size));
    if (driver.gcount() != static_cast<std::streamsize>(driver_size) ||
        inf.gcount() != static_cast<std::streamsize>(inf_size) || driver.bad() || inf.bad() ||
        driver.peek() != std::char_traits<char>::eof() || inf.peek() != std::char_traits<char>::eof() ||
        !rtl8125::package::PeView(bytes).valid() || !rtl8125::package::operational_variant(bytes) ||
        !rtl8125::package::validate_inf(text)) {
        std::fprintf(stderr, "PE32 native/import, operational build marker or exact-device INF check failed\n");
        return 1;
    }
    std::puts("Package metadata passes: i386 native PE, executable entry, operational build marker, NDIS/kernel-only imports, exact PCI/service INF");
    std::puts("This check does not establish successful guest installation or networking");
    return 0;
}
