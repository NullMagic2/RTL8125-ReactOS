// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.hardware.phy
Purpose: Preserve Linux RTL8125B tuning order without Linux phylib assumptions.
Created: 2026-10-24
Architecture: All paged accesses use one serialized bounded register session.
Derived from Linux v6.12 r8169_phy_config.c, Heiner Kallweit and contributors.
*/
#include "phy.hpp"

namespace rtl8125::hardware {
namespace {
struct EphyEntry { std::uint16_t reg, clear, set; };
constexpr EphyEntry a_entries[] = {
    {0x04, 0xffff, 0xd000}, {0x0a, 0xffff, 0x8653}, {0x23, 0xffff, 0xab66},
    {0x20, 0xffff, 0x9455}, {0x21, 0xffff, 0x99ff}, {0x29, 0xffff, 0xfe04},
    {0x44, 0xffff, 0xd000}, {0x4a, 0xffff, 0x8653}, {0x63, 0xffff, 0xab66},
    {0x60, 0xffff, 0x9455}, {0x61, 0xffff, 0x99ff}, {0x69, 0xffff, 0xfe04}
};
constexpr EphyEntry b_entries[] = {
    {0x0b, 0xffff, 0xa908}, {0x1e, 0xffff, 0x20eb}, {0x4b, 0xffff, 0xa908},
    {0x5e, 0xffff, 0x20eb}, {0x22, 0x0030, 0x0020}, {0x62, 0x0030, 0x0020}
};

bool modify(RegisterSession& io, std::uint16_t reg, std::uint16_t clear,
            std::uint16_t set) noexcept
{
    std::uint16_t previous{};
    return io.mdio_read(reg, previous) && io.mdio_write(reg, (previous & ~clear) | set);
}

bool paged_modify(RegisterSession& io, std::uint16_t page, std::uint16_t reg,
                  std::uint16_t clear, std::uint16_t set) noexcept
{
    std::uint16_t previous_page{};
    return io.mdio_read(0x1f, previous_page) && io.mdio_write(0x1f, page) &&
        modify(io, reg, clear, set) && io.mdio_write(0x1f, previous_page);
}

bool parameter(RegisterSession& io, std::uint16_t parameter_address,
               std::uint16_t clear, std::uint16_t set) noexcept
{
    std::uint16_t previous_page{};
    return io.mdio_read(0x1f, previous_page) && io.mdio_write(0x1f, 0xa43) &&
        io.mdio_write(0x13, parameter_address) && modify(io, 0x14, clear, set) &&
        io.mdio_write(0x1f, previous_page);
}
bool paged_write(RegisterSession& io, std::uint16_t page, std::uint16_t reg,
                 std::uint16_t value) noexcept
{
    std::uint16_t previous{};
    return io.mdio_read(0x1f, previous) && io.mdio_write(0x1f, page) &&
        io.mdio_write(reg, value) && io.mdio_write(0x1f, previous);
}

bool apply_firmware(RegisterSession& io, const FirmwareView& firmware) noexcept
{
    const auto result = execute_firmware(firmware, io);
    if (result.status != FirmwareStatus::ok) {
        return io.fail(Status::invalid_argument);
    }
    if (!io.mdio_write(0x1f, 0)) { return false; }
    for (unsigned attempt = 0; attempt < 12; ++attempt) {
        std::uint16_t bmcr{};
        if (!io.delay(50000) || !io.mdio_read(0, bmcr)) { return false; }
        if ((bmcr & 0x8000) == 0) { return true; }
    }
    return io.fail(Status::timeout);
}

}

bool configure_ephy(RegisterSession& io, MacVersion version) noexcept
{
    const EphyEntry* entries{};
    std::size_t count{};
    if (version == MacVersion::rtl8125a) {
        entries = a_entries;
        count = sizeof(a_entries) / sizeof(a_entries[0]);
    } else if (version == MacVersion::rtl8125b) {
        entries = b_entries;
        count = sizeof(b_entries) / sizeof(b_entries[0]);
    } else {
        return io.fail(Status::invalid_argument);
    }
    for (std::size_t index = 0; index < count; ++index) {
        std::uint16_t previous{};
        if (!io.ephy_read(entries[index].reg, previous) ||
            !io.ephy_write(entries[index].reg, (previous & ~entries[index].clear) | entries[index].set)) {
            return false;
        }
    }
    return true;
}

bool configure_rtl8125a_phy(RegisterSession& io, const FirmwareView& firmware) noexcept
{
    if (!paged_modify(io, 0xad4, 0x17, 0, 0x0010) ||
        !paged_modify(io, 0xad1, 0x13, 0x03ff, 0x03ff) ||
        !paged_modify(io, 0xad3, 0x11, 0x003f, 0x0006) ||
        !paged_modify(io, 0xac0, 0x14, 0x1100, 0) ||
        !paged_modify(io, 0xacc, 0x10, 0x0003, 0x0002) ||
        !paged_modify(io, 0xad4, 0x10, 0x00e7, 0x0044) ||
        !paged_modify(io, 0xac1, 0x12, 0x0080, 0) ||
        !paged_modify(io, 0xac8, 0x10, 0x0300, 0) ||
        !paged_modify(io, 0xac5, 0x17, 0x0007, 0x0002) ||
        !paged_write(io, 0xad4, 0x16, 0x00a8) ||
        !paged_write(io, 0xac5, 0x16, 0x01ff) ||
        !paged_modify(io, 0xac8, 0x15, 0x00f0, 0x0030) ||
        !io.mdio_write(0x1f, 0xb87) || !io.mdio_write(0x16, 0x80a2) ||
        !io.mdio_write(0x17, 0x0153) || !io.mdio_write(0x16, 0x809c) ||
        !io.mdio_write(0x17, 0x0153) || !io.mdio_write(0x1f, 0) ||
        !io.mdio_write(0x1f, 0xa43) || !io.mdio_write(0x13, 0x81b3)) {
        return false;
    }
    constexpr std::uint16_t curve[] = {0x0043, 0x00a7, 0x00d6, 0x00ec, 0x00f6,
        0x00fb, 0x00fd, 0x00ff, 0x00bb, 0x0058, 0x0029, 0x0013, 0x0009, 0x0004, 0x0002};
    for (const auto value : curve) {
        if (!io.mdio_write(0x14, value)) { return false; }
    }
    for (unsigned index = 0; index < 25; ++index) {
        if (!io.mdio_write(0x14, 0)) { return false; }
    }
    return io.mdio_write(0x1f, 0) && parameter(io, 0x8257, 0xffff, 0x020f) &&
        parameter(io, 0x80ea, 0xffff, 0x7843) && apply_firmware(io, firmware) &&
        paged_modify(io, 0xd06, 0x14, 0, 0x2000) && parameter(io, 0x81a2, 0, 0x0100) &&
        paged_modify(io, 0xb54, 0x16, 0xff00, 0xdb00) &&
        paged_modify(io, 0xa45, 0x12, 1, 0) && paged_modify(io, 0xa5d, 0x12, 0, 0x0020) &&
        paged_modify(io, 0xad4, 0x17, 0x0010, 0) && paged_modify(io, 0xa86, 0x15, 1, 0) &&
        paged_modify(io, 0xa44, 0x11, 0, 0x0800) &&
        paged_modify(io, 0xa43, 0x10, 4, 0) &&
        paged_modify(io, 0xa43, 0x11, 0, 0x0010) &&
        paged_modify(io, 0xa4a, 0x11, 0, 0x0200) &&
        paged_modify(io, 0xa42, 0x14, 0, 0x0080) &&
        paged_modify(io, 0xa6d, 0x12, 1, 0) && paged_modify(io, 0xa6d, 0x14, 0x0010, 0);
}

bool configure_rtl8125b_phy(RegisterSession& io, const FirmwareView& firmware) noexcept
{
    if (!apply_firmware(io, firmware)) { return false; }
    if (!paged_modify(io, 0xa44, 0x11, 0, 0x0800) ||
        !paged_modify(io, 0xac4, 0x13, 0x00f0, 0x0090) ||
        !paged_modify(io, 0xad3, 0x10, 0x0003, 0x0001) ||
        !io.mdio_write(0x1f, 0xb87) || !io.mdio_write(0x16, 0x80f5) ||
        !io.mdio_write(0x17, 0x760e) || !io.mdio_write(0x16, 0x8107) ||
        !io.mdio_write(0x17, 0x360e) || !io.mdio_write(0x16, 0x8551) ||
        !modify(io, 0x17, 0xff00, 0x0800) || !io.mdio_write(0x1f, 0) ||
        !paged_modify(io, 0xbf0, 0x10, 0xe000, 0xa000) ||
        !paged_modify(io, 0xbf4, 0x13, 0x0f00, 0x0300)) {
        return false;
    }
    for (std::uint16_t address = 0x8044; address <= 0x807a; address += 6) {
        if (!parameter(io, address, 0xffff, 0x2417)) {
            return false;
        }
    }
    return paged_modify(io, 0xa4c, 0x15, 0, 0x0040) &&
        paged_modify(io, 0xbf8, 0x12, 0xe000, 0xa000) &&
        paged_modify(io, 0xa5b, 0x12, 0x8000, 0) &&
        paged_modify(io, 0xa43, 0x10, 0x0004, 0) &&
        paged_modify(io, 0xa6d, 0x12, 0x0001, 0) &&
        paged_modify(io, 0xa6d, 0x14, 0x0010, 0) &&
        paged_modify(io, 0xa42, 0x14, 0x0080, 0) &&
        paged_modify(io, 0xa4a, 0x11, 0x0200, 0);
}

} // namespace rtl8125::hardware
