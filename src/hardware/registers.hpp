// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.hardware.registers
Purpose: Bounded serialized RTL8125 indirect register transactions.
Created: 2026-10-24
Architecture: Portable algorithms over checked platform I/O; no native mapping.
Derived from Linux v6.12 r8169_main.c; see THIRD_PARTY.txt.
*/
#pragma once

#include <cstddef>
#include <cstdint>

namespace rtl8125::hardware {

enum class Status { ok, invalid_argument, busy, io_error, removed, timeout, budget_exhausted };

struct RegisterIo {
    void* context{};
    bool (*claim)(void*) noexcept{};
    void (*release)(void*) noexcept{};
    bool (*read)(void*, std::uint32_t, unsigned, std::uint32_t&) noexcept{};
    bool (*write)(void*, std::uint32_t, unsigned, std::uint32_t) noexcept{};
    bool (*delay_us)(void*, std::uint32_t) noexcept{};
    std::uint64_t (*now_us)(void*) noexcept{};
    bool (*pci_read)(void*, std::uint32_t, unsigned, std::uint32_t&) noexcept{};
    bool (*pci_write)(void*, std::uint32_t, unsigned, std::uint32_t) noexcept{};
    std::uint8_t pci_function{};
};

struct AccessBudget {
    std::uint32_t operations{200000};
    std::uint32_t delay_us{2000000};
    std::uint32_t elapsed_us{5000000};
};

// PASSIVE_LEVEL in the native adapter. Acquires the platform serialization owner
// for the whole sequence, including MDIO page state and read/modify/write pairs.
// The context, callbacks and mapping outlive this noncopyable scope. Callbacks
// must perform bounded I/O and delays. Failed access latches the first error;
// further operations perform no I/O. Time and operation caps bound every scope.
class RegisterSession {
public:
    explicit RegisterSession(const RegisterIo&, AccessBudget = {}) noexcept;
    ~RegisterSession() noexcept;
    RegisterSession(const RegisterSession&) = delete;
    RegisterSession& operator=(const RegisterSession&) = delete;
    Status status() const noexcept { return status_; }
    bool checkpoint() noexcept { return consume(); }
    bool read(std::uint32_t offset, unsigned width, std::uint32_t& value) noexcept;
    bool write(std::uint32_t offset, unsigned width, std::uint32_t value) noexcept;
    bool delay(std::uint32_t microseconds) noexcept;
    bool fail(Status) noexcept;
    bool pci_read(std::uint32_t offset, unsigned width, std::uint32_t&) noexcept;
    bool pci_write(std::uint32_t offset, unsigned width, std::uint32_t) noexcept;
    // Extended config rejection is expected on legacy NT; falls back to CSI.
    bool set_aspm_latency(std::uint8_t) noexcept;
    bool csi_read(std::uint16_t, std::uint32_t&) noexcept;
    bool csi_write(std::uint16_t, std::uint32_t) noexcept;
    bool mac_read(std::uint32_t address, std::uint16_t& value) noexcept;
    bool mac_write(std::uint32_t address, std::uint16_t value) noexcept;
    bool mac_modify(std::uint32_t address, std::uint16_t clear, std::uint16_t set) noexcept;
    bool phy_read(std::uint32_t address, std::uint16_t& value) noexcept;
    bool phy_write(std::uint32_t address, std::uint16_t value) noexcept;
    bool ephy_read(std::uint16_t index, std::uint16_t& value) noexcept;
    bool ephy_write(std::uint16_t index, std::uint16_t value) noexcept;
    bool eri_read(std::uint16_t address, std::uint32_t& value) noexcept;
    bool eri_write(std::uint16_t address, std::uint8_t byte_mask, std::uint32_t value) noexcept;
    bool mdio_read(std::uint16_t reg, std::uint16_t& value) noexcept;
    bool mdio_write(std::uint16_t reg, std::uint16_t value) noexcept;
    bool mcu_read(std::uint16_t reg, std::uint16_t& value) noexcept;
    bool mcu_write(std::uint16_t reg, std::uint16_t value) noexcept;
    bool poll(std::uint32_t offset, std::uint32_t mask, bool set,
              std::uint32_t delay_us, std::uint32_t attempts, std::uint32_t& value) noexcept;

private:
    bool consume() noexcept;
    bool valid_ocp(std::uint32_t address) noexcept;
    bool mdio_address(std::uint16_t reg, std::uint32_t& address) noexcept;
    RegisterIo io_{};
    AccessBudget remaining_{};
    std::uint64_t start_us_{};
    std::uint32_t ocp_base_{0xa400};
    Status status_{Status::invalid_argument};
    bool owned_{};
};

} // namespace rtl8125::hardware
