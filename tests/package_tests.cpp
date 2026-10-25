/*
Module: rtl8125.tests.package_boundaries
Purpose: Reject malformed PE mappings and unsafe INF bindings before package staging.
Created: 2026-10-24
Architecture: Portable modeled metadata tests, separate from actual PE/guest evidence.
*/
#include "../src/installer/package_contract.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace {
unsigned checks{};
unsigned failures{};
void check(bool result)
{
    ++checks;
    if (!result) {
        ++failures;
        std::fprintf(stderr, "package check %u failed\n", checks);
    }
}
void put16(std::vector<std::uint8_t>& bytes, std::size_t offset, unsigned value)
{
    bytes[offset] = value & 255;
    bytes[offset + 1] = (value >> 8) & 255;
}
void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, unsigned value)
{
    put16(bytes, offset, value);
    put16(bytes, offset + 2, value >> 16);
}
std::vector<std::uint8_t> fixture()
{
    std::vector<std::uint8_t> bytes(1024);
    put16(bytes, 0, 0x5a4d);
    put32(bytes, 0x3c, 128);
    put32(bytes, 128, 0x4550);
    put16(bytes, 132, 0x14c);
    put16(bytes, 134, 1);
    put16(bytes, 148, 224);
    put16(bytes, 150, 0x102);
    put16(bytes, 152, 0x10b);
    put32(bytes, 168, 0x1000);
    put16(bytes, 220, 1);
    put32(bytes, 244, 16);
    put32(bytes, 256, 0x1040);
    put32(bytes, 260, 60);
    put32(bytes, 388, 0x1000);
    put32(bytes, 392, 512);
    put32(bytes, 396, 512);
    put32(bytes, 412, 0x20000000);
    put32(bytes, 576 + 12, 0x10c0);
    put32(bytes, 596 + 12, 0x10d0);
    const std::string ndis = "ndis.sys";
    const std::string kernel = "ntoskrnl.exe";
    std::copy(ndis.begin(), ndis.end(), bytes.begin() + 704);
    std::copy(kernel.begin(), kernel.end(), bytes.begin() + 720);
    return bytes;
}
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    const std::string inf((std::istreambuf_iterator<char>(input)), {});
    check(input.good() && rtl8125::package::validate_inf(inf));
    for (const auto* replacement : {"PCI\\VEN_10EC&DEV_8139", "PCI\\VEN_10EC", "PCI\\CC_0200"}) {
        auto value = inf;
        const auto position = value.find("PCI\\VEN_10EC&DEV_8125");
        value.replace(position, 21, replacement);
        check(!rtl8125::package::validate_inf(value));
    }
    for (const auto* replacement : {"StartType=0", "StartType=1", "StartType=2"}) {
        auto value = inf;
        value.replace(value.find("StartType=3"), 11, replacement);
        check(!rtl8125::package::validate_inf(value));
    }
    check(!rtl8125::package::validate_inf(inf + "\n[Devices]\n"));
    check(!rtl8125::package::validate_inf(inf + "\n[DefaultInstall]\nCopyFiles=RTL8125_Files\n"));
    check(!rtl8125::package::validate_inf(inf + std::string(1, '\0')));
    check(!rtl8125::package::validate_inf(std::string(65537, 'x')));
    const auto bytes = fixture();
    check(rtl8125::package::PeView(bytes).valid());
    check(!rtl8125::package::operational_variant(bytes));
    auto marked = bytes;
    constexpr char marker[] = "RTL8125: operational adapter initialized\n";
    marked.insert(marked.end(), marker, marker + sizeof(marker));
    check(rtl8125::package::operational_variant(marked));
    constexpr char probe[] = "RTL8125: probe XID %03lx, MAC family %lu; datapath incomplete\n";
    marked.insert(marked.end(), probe, probe + sizeof(probe));
    check(!rtl8125::package::operational_variant(marked));
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + size);
        check(!rtl8125::package::PeView(truncated).valid());
    }
    for (const auto field : {0U, 0x3cU, 128U, 132U, 134U, 148U, 150U, 152U,
            168U, 220U, 244U, 256U, 260U, 388U, 392U, 396U, 412U, 588U, 608U}) {
        auto value = bytes;
        put32(value, field, field == 412 ? 0 : 0xffffffff);
        check(!rtl8125::package::PeView(value).valid());
    }
    auto value = bytes;
    value[704] = 'v';
    check(!rtl8125::package::PeView(value).valid());
    value = bytes;
    put32(value, 152 + 96 + 9 * 8, 0x1000);
    check(!rtl8125::package::PeView(value).valid());
    value = bytes;
    put32(value, 152 + 96 + 14 * 8, 0x1000);
    check(!rtl8125::package::PeView(value).valid());
    for (const std::string section : {".CRT", ".ctors", ".dtors", ".tls"}) {
        value = bytes;
        std::copy(section.begin(), section.end(), value.begin() + 376);
        check(!rtl8125::package::PeView(value).valid());
    }
    std::printf("package metadata: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
