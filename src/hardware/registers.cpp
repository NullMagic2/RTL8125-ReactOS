// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.hardware.registers
Purpose: Translate Linux OCP, EPHY, ERI and paged MDIO with explicit failures.
Created: 2026-10-24
Architecture: RegisterSession owns exclusive sequence access, timing and budget.
Derived from Linux v6.12 r8169_main.c, Francois Romieu et al.
*/
#include "registers.hpp"

namespace rtl8125::hardware {
namespace {
constexpr std::uint32_t busy_bit = UINT32_C(1) << 31;
constexpr std::uint32_t mac_port = 0xb0;
constexpr std::uint32_t phy_port = 0xb8;
constexpr std::uint32_t ephy_port = 0x80;
constexpr std::uint32_t eri_port = 0x74;
constexpr std::uint32_t eri_data = 0x70;
}

RegisterSession::RegisterSession(const RegisterIo& io, AccessBudget budget) noexcept
    : io_(io), remaining_(budget)
{
    if (io.context == nullptr || io.claim == nullptr || io.release == nullptr ||
        io.read == nullptr || io.write == nullptr || io.delay_us == nullptr ||
        io.now_us == nullptr || budget.operations == 0 || budget.operations > 1000000 ||
        budget.delay_us > 10000000 || budget.elapsed_us == 0 || budget.elapsed_us > 30000000) {
        return;
    }
    if (!io_.claim(io_.context)) {
        status_ = Status::busy;
        return;
    }
    owned_ = true;
    status_ = Status::ok;
    start_us_ = io_.now_us(io_.context);
}

RegisterSession::~RegisterSession() noexcept
{
    if (owned_) {
        io_.release(io_.context);
    }
}

bool RegisterSession::fail(Status status) noexcept
{
    if (status_ == Status::ok) {
        status_ = status;
    }
    return false;
}

bool RegisterSession::consume() noexcept
{
    if (status_ != Status::ok) {
        return false;
    }
    if (remaining_.operations == 0) {
        return fail(Status::budget_exhausted);
    }
    const auto current = io_.now_us(io_.context);
    if (current < start_us_ || current - start_us_ >= remaining_.elapsed_us) {
        return fail(Status::timeout);
    }
    --remaining_.operations;
    return true;
}

bool RegisterSession::read(std::uint32_t offset, unsigned width, std::uint32_t& value) noexcept
{
    value = 0;
    if ((width != 1 && width != 2 && width != 4) || offset % width != 0) {
        return fail(Status::invalid_argument);
    }
    if (!consume()) {
        return false;
    }
    if (!io_.read(io_.context, offset, width, value)) {
        value = 0;
        return fail(Status::io_error);
    }
    return true;
}

bool RegisterSession::write(std::uint32_t offset, unsigned width, std::uint32_t value) noexcept
{
    if ((width != 1 && width != 2 && width != 4) || offset % width != 0 ||
        (width < 4 && value >= (UINT32_C(1) << (width * 8)))) {
        return fail(Status::invalid_argument);
    }
    return consume() && (io_.write(io_.context, offset, width, value) || fail(Status::io_error));
}

bool RegisterSession::delay(std::uint32_t microseconds) noexcept
{
    if (!consume()) {
        return false;
    }
    if (microseconds > remaining_.delay_us) {
        return fail(Status::budget_exhausted);
    }
    remaining_.delay_us -= microseconds;
    if (!io_.delay_us(io_.context, microseconds)) {
        return fail(Status::io_error);
    }
    return consume();
}

bool RegisterSession::poll(std::uint32_t offset, std::uint32_t mask, bool set,
                           std::uint32_t pause, std::uint32_t attempts,
                           std::uint32_t& value) noexcept
{
    value = 0;
    if (mask == 0 || attempts == 0 || attempts > 10000 || pause > 100000) {
        return fail(Status::invalid_argument);
    }
    // Linux rtl_loop_wait delays before each read, including the first read.
    for (std::uint32_t index = 0; index < attempts; ++index) {
        if (!delay(pause) || !read(offset, 4, value)) {
            value = 0;
            return false;
        }
        if (value == UINT32_MAX) {
            value = 0;
            return fail(Status::removed);
        }
        if (((value & mask) != 0) == set) {
            return true;
        }
    }
    value = 0;
    return fail(Status::timeout);
}

bool RegisterSession::pci_read(std::uint32_t offset, unsigned width, std::uint32_t& value) noexcept
{
    value = 0;
    if ((width != 1 && width != 2 && width != 4) || offset % width != 0 ||
        offset > 4096 - width || io_.pci_read == nullptr) {
        return fail(Status::invalid_argument);
    }
    std::uint32_t temporary{};
    if (!consume() || !io_.pci_read(io_.context, offset, width, temporary)) {
        return fail(Status::io_error);
    }
    value = temporary;
    return true;
}

bool RegisterSession::pci_write(std::uint32_t offset, unsigned width, std::uint32_t value) noexcept
{
    if ((width != 1 && width != 2 && width != 4) || offset % width != 0 ||
        offset > 4096 - width || io_.pci_write == nullptr ||
        (width < 4 && value >= (UINT32_C(1) << (width * 8)))) {
        return fail(Status::invalid_argument);
    }
    return consume() && (io_.pci_write(io_.context, offset, width, value) || fail(Status::io_error));
}

bool RegisterSession::csi_read(std::uint16_t address, std::uint32_t& value) noexcept
{
    value = 0;
    std::uint32_t command{};
    if (address > 0xffc || (address & 3) != 0 || io_.pci_function > 7) {
        return fail(Status::invalid_argument);
    }
    return write(0x68, 4, address | 0xf000u | (std::uint32_t(io_.pci_function) << 16)) &&
        poll(0x68, busy_bit, true, 10, 100, command) && read(0x64, 4, value);
}

bool RegisterSession::csi_write(std::uint16_t address, std::uint32_t value) noexcept
{
    std::uint32_t command{};
    if (address > 0xffc || (address & 3) != 0 || io_.pci_function > 7) {
        return fail(Status::invalid_argument);
    }
    return write(0x64, 4, value) && write(0x68, 4,
        busy_bit | address | 0xf000u | (std::uint32_t(io_.pci_function) << 16)) &&
        poll(0x68, busy_bit, false, 10, 100, command);
}

bool RegisterSession::set_aspm_latency(std::uint8_t value) noexcept
{
    if (!consume()) { return false; }
    if (io_.pci_write != nullptr && io_.pci_write(io_.context, 0x70f, 1, value)) {
        return true;
    }
    std::uint32_t previous{};
    return csi_read(0x70c, previous) && csi_write(0x70c,
        (previous & 0x00ffffff) | (std::uint32_t(value) << 24));
}

bool RegisterSession::valid_ocp(std::uint32_t address) noexcept
{
    return (address <= 0xfffe && (address & 1) == 0) || fail(Status::invalid_argument);
}

bool RegisterSession::mac_read(std::uint32_t address, std::uint16_t& value) noexcept
{
    value = 0;
    std::uint32_t raw{};
    if (!valid_ocp(address) || !write(mac_port, 4, address << 15) || !read(mac_port, 4, raw)) {
        return false;
    }
    if (raw == UINT32_MAX) {
        return fail(Status::removed);
    }
    value = static_cast<std::uint16_t>(raw);
    return true;
}

bool RegisterSession::mac_write(std::uint32_t address, std::uint16_t value) noexcept
{
    return valid_ocp(address) && write(mac_port, 4, busy_bit | (address << 15) | value);
}

bool RegisterSession::mac_modify(std::uint32_t address, std::uint16_t clear,
                                 std::uint16_t set) noexcept
{
    std::uint16_t previous{};
    return mac_read(address, previous) && mac_write(address, (previous & ~clear) | set);
}

bool RegisterSession::phy_read(std::uint32_t address, std::uint16_t& value) noexcept
{
    value = 0;
    std::uint32_t raw{};
    if (!valid_ocp(address) || !write(phy_port, 4, address << 15) ||
        !poll(phy_port, busy_bit, true, 25, 10, raw)) {
        return false;
    }
    value = static_cast<std::uint16_t>(raw);
    return true;
}

bool RegisterSession::phy_write(std::uint32_t address, std::uint16_t value) noexcept
{
    std::uint32_t raw{};
    return valid_ocp(address) && write(phy_port, 4, busy_bit | (address << 15) | value) &&
        poll(phy_port, busy_bit, false, 25, 10, raw);
}

bool RegisterSession::ephy_read(std::uint16_t index, std::uint16_t& value) noexcept
{
    value = 0;
    std::uint32_t raw{};
    // Linux masks the bank bits in the A/B tables with EPHYAR_REG_MASK.
    if (index > 0x7f) {
        return fail(Status::invalid_argument);
    }
    if (!write(ephy_port, 4, static_cast<std::uint32_t>(index & 0x1f) << 16) ||
        !poll(ephy_port, busy_bit, true, 10, 100, raw)) {
        return false;
    }
    value = static_cast<std::uint16_t>(raw);
    return true;
}

bool RegisterSession::ephy_write(std::uint16_t index, std::uint16_t value) noexcept
{
    if (index > 0x7f) {
        return fail(Status::invalid_argument);
    }
    std::uint32_t raw{};
    return write(ephy_port, 4, busy_bit | (static_cast<std::uint32_t>(index & 0x1f) << 16) | value) &&
        poll(ephy_port, busy_bit, false, 10, 100, raw) && delay(10);
}

bool RegisterSession::eri_read(std::uint16_t address, std::uint32_t& value) noexcept
{
    value = 0;
    std::uint32_t raw{};
    if (address > 0xffc || (address & 3) != 0) {
        return fail(Status::invalid_argument);
    }
    return write(eri_port, 4, 0xf000 | address) &&
        poll(eri_port, busy_bit, true, 100, 100, raw) && read(eri_data, 4, value);
}

bool RegisterSession::eri_write(std::uint16_t address, std::uint8_t byte_mask,
                                std::uint32_t value) noexcept
{
    if (address > 0xffc || (address & 3) != 0 || byte_mask == 0 || byte_mask > 0xf) {
        return fail(Status::invalid_argument);
    }
    std::uint32_t raw{};
    return write(eri_data, 4, value) &&
        write(eri_port, 4, busy_bit | (static_cast<std::uint32_t>(byte_mask) << 12) | address) &&
        poll(eri_port, busy_bit, false, 100, 100, raw);
}

bool RegisterSession::mdio_address(std::uint16_t reg, std::uint32_t& address) noexcept
{
    if (reg > 0x1e) {
        return fail(Status::invalid_argument);
    }
    // Linux subtracts 0x10 on nonstandard pages, including negative values.
    const auto adjusted = static_cast<std::int32_t>(reg) - (ocp_base_ == 0xa400 ? 0 : 16);
    const auto result = static_cast<std::int32_t>(ocp_base_) + adjusted * 2;
    if (result < 0 || result > 0xfffe) {
        return fail(Status::invalid_argument);
    }
    address = static_cast<std::uint32_t>(result);
    return true;
}

bool RegisterSession::mdio_read(std::uint16_t reg, std::uint16_t& value) noexcept
{
    value = 0;
    if (reg == 0x1f) {
        if (!consume()) {
            return false;
        }
        value = ocp_base_ == 0xa400 ? 0 : static_cast<std::uint16_t>(ocp_base_ >> 4);
        return true;
    }
    std::uint32_t address{};
    return mdio_address(reg, address) && phy_read(address, value);
}

bool RegisterSession::mdio_write(std::uint16_t reg, std::uint16_t value) noexcept
{
    if (reg == 0x1f) {
        if (value > 0xfff || !consume()) {
            return fail(Status::invalid_argument);
        }
        ocp_base_ = value == 0 ? 0xa400 : static_cast<std::uint32_t>(value) << 4;
        return true;
    }
    std::uint32_t address{};
    return mdio_address(reg, address) && phy_write(address, value);
}

bool RegisterSession::mcu_read(std::uint16_t reg, std::uint16_t& value) noexcept
{
    return mac_read(ocp_base_ + reg, value);
}

bool RegisterSession::mcu_write(std::uint16_t reg, std::uint16_t value) noexcept
{
    if (reg == 0x1f) {
        if (value > 0xfff || !consume()) {
            return fail(Status::invalid_argument);
        }
        ocp_base_ = static_cast<std::uint32_t>(value) << 4;
        return true;
    }
    return mac_write(ocp_base_ + reg, value);
}

} // namespace rtl8125::hardware
