// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.hardware.device
Purpose: Preserve pinned Linux RTL8125 initialization and truthful link state.
Created: 2026-10-24
Architecture: Bounded exclusive sequence; activation and retirement are separate.
Derived from Linux v6.12 r8169_main.c, r8169_phy_config.c and realtek_phy.c.
*/
#include "device.hpp"

namespace rtl8125::hardware {
namespace {
constexpr std::uint32_t ring_bytes = dma_descriptor_count * legacy_descriptor_size;
bool modify(RegisterSession& io, std::uint32_t offset, unsigned width,
            std::uint32_t clear, std::uint32_t set) noexcept
{
    std::uint32_t previous{};
    if (!io.read(offset, width, previous)) { return false; }
    const auto absent = width == 1 ? 0xffu : width == 2 ? 0xffffu : UINT32_MAX;
    return previous != absent ? io.write(offset, width, (previous & ~clear) | set) : io.fail(Status::removed);
}
bool wait(RegisterSession& io, std::uint32_t offset, unsigned width,
          std::uint32_t mask, std::uint32_t expected,
          std::uint32_t pause, unsigned attempts) noexcept
{
    for (unsigned index = 0; index < attempts; ++index) {
        std::uint32_t value{};
        if (!io.delay(pause) || !io.read(offset, width, value)) { return false; }
        const auto absent = width == 1 ? 0xffu : width == 2 ? 0xffffu : UINT32_MAX;
        if (value == absent) { return io.fail(Status::removed); }
        if ((value & mask) == expected) { return true; }
    }
    return io.fail(Status::timeout);
}
bool commit(RegisterSession& io) noexcept
{
    std::uint32_t value{};
    return io.read(0x37, 1, value) && (value != 0xff || io.fail(Status::removed));
}
bool paged_modify(RegisterSession& io, std::uint16_t page, std::uint16_t reg,
                  std::uint16_t clear, std::uint16_t set) noexcept
{
    std::uint16_t previous_page{}, value{};
    return io.mdio_read(0x1f, previous_page) && io.mdio_write(0x1f, page) &&
        io.mdio_read(reg, value) && io.mdio_write(reg, (value & ~clear) | set) &&
        io.mdio_write(0x1f, previous_page);
}
bool phy_reset(RegisterSession& io) noexcept
{
    std::uint16_t value{};
    if (!io.mdio_write(0x1f, 0) || !io.mdio_read(0, value) ||
        !io.mdio_write(0, (value & ~0x0800) | 0x8000)) { return false; }
    for (unsigned index = 0; index < 12; ++index) {
        if (!io.delay(50000) || !io.mdio_read(0, value)) { return false; }
        if ((value & 0x8000) == 0) { return true; }
    }
    return io.fail(Status::timeout);
}
bool pci_setup(RegisterSession& io) noexcept
{
    std::uint32_t capabilities{}, pointer{};
    if (!io.pci_read(6, 2, capabilities) || (capabilities & 0x10) == 0 ||
        !io.pci_read(0x34, 1, pointer)) { return io.fail(Status::invalid_argument); }
    std::uint64_t visited{};
    for (unsigned count = 0; count < 48; ++count) {
        if (pointer < 0x40 || pointer > 0xfc || (pointer & 3) != 0) {
            return io.fail(Status::invalid_argument);
        }
        const auto bit = UINT64_C(1) << (pointer / 4);
        if ((visited & bit) != 0) { return io.fail(Status::invalid_argument); }
        visited |= bit;
        std::uint32_t header{};
        if (!io.pci_read(pointer, 2, header)) { return false; }
        if ((header & 0xff) == 0x10) {
            if (pointer > 0xe8) { return io.fail(Status::invalid_argument); }
            std::uint32_t link{}, control{};
            // PCIe L1/clock request stay disabled until OS policy is qualified.
            std::uint32_t verified_link{}, verified_control{};
            return io.pci_read(pointer + 0x10, 2, link) &&
                io.pci_write(pointer + 0x10, 2, link & ~0x0103u) &&
                io.pci_read(pointer + 0x10, 2, verified_link) &&
                ((verified_link & 0x0103) == 0 || io.fail(Status::io_error)) &&
                io.pci_read(pointer + 8, 2, control) &&
                io.pci_write(pointer + 8, 2, (control & ~0x7000u) | 0x5000) &&
                io.pci_read(pointer + 8, 2, verified_control) &&
                ((verified_control & 0x7000) == 0x5000 || io.fail(Status::io_error));
        }
        pointer = header >> 8;
    }
    return io.fail(Status::invalid_argument);
}
bool aspm_off(RegisterSession& io) noexcept
{
    return io.mac_modify(0xe092, 0x00ff, 0) && modify(io, 0x53, 1, 0x80, 0) &&
        modify(io, 0x56, 1, 1, 0);
}
bool oob_initialize(RegisterSession& io, MacVersion version) noexcept
{
    if (!modify(io, 0xf0, 4, 0, 0x00080000) || !io.delay(2000)) { return false; }
    if (version == MacVersion::rtl8125b && !modify(io, 0x37, 1, 0, 0x80)) { return false; }
    if (!wait(io, 0xd3, 1, 0x30, 0x30, 100, 42)) { return false; }
    if (version == MacVersion::rtl8125b && !wait(io, 0xe2, 2, 0x0103, 0x0103, 100, 42)) {
        return false;
    }
    return modify(io, 0x37, 1, 0x0c, 0) && io.delay(1000) &&
        modify(io, 0xd3, 1, 0x80, 0) && io.mac_modify(0xe8de, 0x4000, 0) &&
        wait(io, 0xd3, 1, 2, 2, 100, 42) && io.mac_write(0xc0aa, 0x07d0) &&
        io.mac_write(0xc0a6, 0x0150) && io.mac_write(0xc01e, 0x5555) &&
        wait(io, 0xd3, 1, 2, 2, 100, 42);
}
bool mac_common(RegisterSession& io, MacVersion version) noexcept
{
    if (!modify(io, 0x54, 1, 2, 0) || !io.write(0x382, 2, 0x221b) ||
        !io.write(0x4500, 1, 0) || !io.write(0x4800, 2, 0) ||
        !io.mac_modify(0xd40a, 0x0010, 0) || !modify(io, 0x52, 1, 0x10, 0) ||
        !io.mac_write(0xc140, 0xffff) || !io.mac_write(0xc142, 0xffff) ||
        !io.mac_modify(0xd3e2, 0x0fff, 0x03a9) || !io.mac_modify(0xd3e4, 0x00ff, 0) ||
        !io.mac_modify(0xe860, 0, 0x0080) || !io.mac_modify(0xeb58, 1, 0) ||
        !io.mac_modify(0xe614, 0x0700, version == MacVersion::rtl8125b ? 0x0200 : 0x0300) ||
        !io.mac_modify(0xe63e, 0x0c30, version == MacVersion::rtl8125b ? 0 : 0x0020) ||
        !io.mac_modify(0xc0b4, 0, 0x000c) || !io.mac_modify(0xeb6a, 0x00ff, 0x0033) ||
        !io.mac_modify(0xeb50, 0x03e0, 0x0040) || !io.mac_modify(0xe056, 0x00f0, 0x0030) ||
        !io.mac_modify(0xe040, 0x1000, 0) || !io.mac_modify(0xea1c, 3, 1) ||
        !io.mac_modify(0xea1c, 4, 0) || !io.mac_modify(0xe0c0, 0x4f0f, 0x4403) ||
        !io.mac_modify(0xe052, 0x0080, 0x0068) || !io.mac_modify(0xd430, 0x0fff, 0x047f) ||
        !io.mac_modify(0xea1c, 4, 0) || !io.mac_modify(0xeb54, 0, 1) || !io.delay(1) ||
        !io.mac_modify(0xeb54, 1, 0) || !modify(io, 0x1880, 2, 0x0030, 0) ||
        !io.mac_write(0xe098, 0xc302)) { return false; }
    bool ready{};
    for (unsigned index = 0; index < 10; ++index) {
        std::uint16_t value{};
        if (!io.delay(1000) || !io.mac_read(0xe00e, value)) { return false; }
        if ((value & 0x2000) == 0) { ready = true; break; }
    }
    if (!ready) { return io.fail(Status::timeout); }
    if (!io.mac_modify(0xe040, 0, 3)) { return false; }
    if (version == MacVersion::rtl8125a && !io.mac_modify(0xeb62, 0, 6)) { return false; }
    return modify(io, 0xf0, 4, 0x00080000, 0);
}
bool read_address(RegisterSession& io, std::uint32_t offset, std::uint8_t* address) noexcept
{
    for (unsigned index = 0; index < 6; ++index) {
        std::uint32_t byte{};
        if (!io.read(offset + index, 1, byte)) { return false; }
        address[index] = static_cast<std::uint8_t>(byte);
    }
    return true;
}
bool valid_address(const std::uint8_t* address) noexcept
{
    unsigned any{};
    for (unsigned index = 0; index < 6; ++index) { any |= address[index]; }
    return (address[0] & 1) == 0 && any != 0;
}
bool station_address(RegisterSession& io, std::uint8_t* address) noexcept
{
    if (!read_address(io, 0x19e0, address)) { return false; }
    if (!valid_address(address) && !read_address(io, 0, address)) { return false; }
    if (!valid_address(address)) { return io.fail(Status::invalid_argument); }
    std::uint32_t low{}, high{};
    for (unsigned index = 0; index < 4; ++index) { low |= std::uint32_t(address[index]) << (index * 8); }
    high = std::uint32_t(address[4]) | (std::uint32_t(address[5]) << 8);
    return io.write(0x50, 1, 0xc0) && io.write(4, 4, high) && commit(io) &&
        io.write(0, 4, low) && commit(io) && io.write(0x50, 1, 0);
}
bool resume_phy(RegisterSession& io) noexcept
{
    std::uint16_t high{}, low{}, capabilities{};
    if (!io.mdio_write(0x1f, 0) || !io.mdio_read(2, high) || !io.mdio_read(3, low)) { return false; }
    const auto id = (std::uint32_t(high) << 16) | low;
    if (id != 0x001cc800 && id != 0x001cc840) { return io.fail(Status::invalid_argument); }
    // Internal RTL8125 PHY uses no external SGMII/2500BASE-X SerDes mode.
    // rtlgen_resume clears BMCR power-down then waits 20 ms before tuning.
    return paged_modify(io, 0, 0, 0x0800, 0) && io.delay(20000) &&
        io.mdio_write(0x1f, 0xa61) && io.mdio_read(0x13, capabilities) &&
        io.mdio_write(0x1f, 0) &&
        ((capabilities != 0xffff && (capabilities & 0x2000) != 0) || io.fail(Status::invalid_argument));
}
bool negotiate(RegisterSession& io) noexcept
{
    // Disable both standard EEE advertisements; preserve unrelated vendor bits.
    // No pause advertisement: initial software packet policy supplies no flow control.
    std::uint16_t value{}, abilities{}, extended{};
    if (!io.mdio_write(0x1f, 0) || !io.mdio_read(1, abilities) ||
        !io.mdio_read(15, extended)) { return false; }
    if (abilities == 0xffff || extended == 0xffff || (abilities & 0x0108) != 0x0108 ||
        (extended & 0x2000) == 0) { return io.fail(Status::invalid_argument); }
    const auto advertised = static_cast<std::uint16_t>(((abilities & 0x7800) >> 6) | 1);
    return paged_modify(io, 0xa5d, 0x10, 0xffff, 0) &&
        paged_modify(io, 0xa6d, 0x12, 0xffff, 0) &&
        paged_modify(io, 0xa5d, 0x12, 0x0180, 0x0080) &&
        io.mdio_write(0x1f, 0) && io.mdio_read(4, value) &&
        io.mdio_write(4, (value & ~0x0fffu) | advertised) &&
        io.mdio_read(9, value) && io.mdio_write(9, (value & ~0x0300u) | 0x0200) &&
        io.mdio_read(0, value) && io.mdio_write(0, (value & ~0x0c00u) | 0x1200);
}
bool reject(DeviceState& state, RegisterSession& io) noexcept
{
    state.initialized = false;
    state.running = false;
    state.recovery_required = true;
    return io.fail(io.status() == Status::ok ? Status::io_error : io.status());
}
}

bool initialize_device(RegisterSession& io, MacVersion version, const FirmwareView& firmware,
                       DeviceState& state) noexcept
{
    if (state.initialized || state.running || state.recovery_required ||
        (version != MacVersion::rtl8125a && version != MacVersion::rtl8125b)) {
        return io.fail(Status::invalid_argument);
    }
    FirmwareView checked{};
    if (validate_firmware(firmware.bytes, firmware.byte_count, checked) != FirmwareStatus::ok) {
        return io.fail(Status::invalid_argument);
    }
    if (io.status() != Status::ok) { return false; }
    std::uint32_t id{};
    if (!io.pci_read(0, 4, id) || id != 0x812510ec) {
        return io.fail(Status::invalid_argument);
    }
    state.version = version;
    state.recovery_required = true;
    const auto rx_config = 0x40000700u | (version == MacVersion::rtl8125b ? 0x800u : 0u);
    if (!pci_setup(io) || !io.write(0x44, 4, rx_config) || !io.write(0x38, 4, 0) ||
        !io.write(0x3c, 4, UINT32_MAX) || !commit(io) || !oob_initialize(io, version) ||
        !io.write(0x37, 1, 0x10) || !wait(io, 0x37, 1, 0x10, 0, 100, 100) ||
        !station_address(io, state.permanent_address) ||
        !resume_phy(io) ||
        !(version == MacVersion::rtl8125a ? configure_rtl8125a_phy(io, firmware) :
                                          configure_rtl8125b_phy(io, firmware)) ||
        !phy_reset(io) || !negotiate(io) || !io.write(0x50, 1, 0xc0) || !aspm_off(io) ||
        !modify(io, 0xe0, 2, 0xdfff, 0x0080) || !io.write(0x6048, 2, 1546) ||
        !io.write(0x34, 1, 0)) { return reject(state, io); }
    const auto end = version == MacVersion::rtl8125a ? 0xb00u : 0xa80u;
    for (std::uint32_t offset = 0xa00; offset < end; offset += 4) {
        if (!io.write(offset, 4, 0)) { return reject(state, io); }
    }
    if ((version == MacVersion::rtl8125b && !io.write(0x7a, 2, 0)) ||
        !io.set_aspm_latency(0x27) || !configure_ephy(io, version) || !mac_common(io, version) ||
        !io.mac_modify(0xc0ac, 0, 0x1f80) || !aspm_off(io) ||
        !io.write(0x50, 1, 0) || !commit(io) || !io.write(0x37, 1, 0) ||
        !io.write(0x38, 4, 0) || !commit(io)) { return reject(state, io); }
    state.initialized = true;
    state.recovery_required = false;
    return true;
}

bool activate_device(RegisterSession& io, DeviceState& state, const RingConfiguration& rings) noexcept
{
    if (!state.initialized || state.running || state.recovery_required ||
        (state.version != MacVersion::rtl8125a && state.version != MacVersion::rtl8125b) ||
        rings.transmit == 0 || rings.receive == 0 || rings.transmit > UINT32_MAX - (ring_bytes - 1) ||
        rings.receive > UINT32_MAX - (ring_bytes - 1) ||
        !(rings.transmit + ring_bytes - 1 < rings.receive || rings.receive + ring_bytes - 1 < rings.transmit) ||
        (rings.transmit & 255) != 0 ||
        (rings.receive & 255) != 0 || rings.receive_buffer_size != dma_buffer_size) { return io.fail(Status::invalid_argument); }
    if (io.status() != Status::ok) { return false; }
    state.recovery_required = true;
    if (!io.write(0x38, 4, 0) || !io.write(0x50, 1, 0xc0) ||
        !io.write(0xda, 2, maximum_frame_size + ethernet_fcs_size + 1) ||
        !io.write(0x24, 4, static_cast<std::uint32_t>(rings.transmit >> 32)) ||
        !io.write(0x20, 4, static_cast<std::uint32_t>(rings.transmit)) ||
        !io.write(0xe8, 4, static_cast<std::uint32_t>(rings.receive >> 32)) ||
        !io.write(0xe4, 4, static_cast<std::uint32_t>(rings.receive)) ||
        !io.write(0x50, 1, 0) || !commit(io) || !io.write(0x37, 1, 0x0c) ||
        !modify(io, 0x44, 4, ~0x0fu,
            0x40000700u | (state.version == MacVersion::rtl8125b ? 0x800u : 0u)) ||
        !io.write(0x40, 4, 0x03000780) || !commit(io)) { return reject(state, io); }
    state.running = true;
    state.recovery_required = false;
    return true;
}

bool read_link(RegisterSession& io, const DeviceState& state, LinkState& output) noexcept
{
    output = {};
    if (!state.initialized || state.recovery_required ||
        (state.version != MacVersion::rtl8125a && state.version != MacVersion::rtl8125b)) {
        return io.fail(Status::invalid_argument);
    }
    std::uint16_t status{}, bmcr{}, speed{}, again{};
    std::uint32_t mac_status{};
    if (!io.mdio_write(0x1f, 0) || !io.mdio_read(1, status) || !io.mdio_read(1, status) ||
        !io.mdio_read(0, bmcr)) { return false; }
    if (status == 0xffff || bmcr == 0xffff) { return io.fail(Status::removed); }
    if ((status & 4) == 0 || (bmcr & 0x1000) == 0 || (status & 0x20) == 0) { return true; }
    if (!io.mdio_write(0x1f, 0xa43) || !io.mdio_read(0x12, speed) ||
        !io.mdio_write(0x1f, 0) || !io.read(0x6c, 1, mac_status) ||
        !io.mdio_read(1, again)) { return false; }
    if (speed == 0xffff || mac_status == 0xff || again == 0xffff) { return io.fail(Status::removed); }
    if ((again & 0x24) != 0x24 || (mac_status & 2) == 0) { return true; }
    switch (speed & 0x0630) {
    case 0: output.speed_mbps = 10; break;
    case 0x10: output.speed_mbps = 100; break;
    case 0x20: output.speed_mbps = 1000; break;
    case 0x210: output.speed_mbps = 2500; break;
    default: return io.fail(Status::invalid_argument);
    }
    output.full_duplex = (mac_status & 1) != 0;
    if (output.speed_mbps >= 1000 && !output.full_duplex) {
        output = {};
        return io.fail(Status::invalid_argument);
    }
    output.up = true;
    output.autoneg_complete = true;
    return true;
}

bool quiesce_device(RegisterSession& io, MacVersion version) noexcept
{
    if (version != MacVersion::rtl8125a && version != MacVersion::rtl8125b) {
        return io.fail(Status::invalid_argument);
    }
    // rtl8169_cleanup / rtl_enable_rxdvgate / rtl_wait_txrx_fifo_empty.
    // RTL8125A is Linux MAC61; RTL8125B is MAC63. TXCFG_EMPTY applies to older
    // 8111 variants, not these devices. OWN-clear is never used as drain proof.
    if (!io.write(0x38, 4, 0) || !modify(io, 0x44, 4, 0x3f, 0) ||
        !modify(io, 0xf0, 4, 0, 0x00080000) || !commit(io) || !io.delay(2000)) {
        return false;
    }
    if (version == MacVersion::rtl8125b && !modify(io, 0x37, 1, 0, 0x80)) {
        return false;
    }
    if (!wait(io, 0xd3, 1, 0x30, 0x30, 100, 42) ||
        (version == MacVersion::rtl8125b &&
         !wait(io, 0xe2, 2, 0x0103, 0x0103, 100, 42)) ||
        !io.delay(2000) || !io.write(0x37, 1, 0x10) ||
        !wait(io, 0x37, 1, 0x1c, 0, 100, 100)) {
        return false;
    }
    return commit(io);
}

bool contain_device(RegisterSession& io, DeviceState& state) noexcept
{
    state.initialized = false;
    state.running = false;
    state.recovery_required = true;
    return io.write(0x38, 4, 0) && modify(io, 0x44, 4, 0x3f, 0) &&
        modify(io, 0x37, 1, 0x0c, 0) && io.write(0x50, 1, 0) && commit(io);
}
} // namespace rtl8125::hardware
