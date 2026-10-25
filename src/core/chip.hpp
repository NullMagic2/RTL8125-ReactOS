// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.chip
Purpose: Identify supported MAC generations using the Linux XID rules.
Created: 2026-10-24
Architecture: Portable core; no device I/O or OS dependencies.
Derived from Linux v6.12 r8169_main.c; see THIRD_PARTY.txt for attribution.
*/
#pragma once

#include <cstdint>

namespace rtl8125 {

inline constexpr std::uint16_t realtek_vendor_id = 0x10ec;
inline constexpr std::uint16_t rtl8125_device_id = 0x8125;
inline constexpr std::uint32_t tx_config_offset = 0x40;

enum class MacVersion { unsupported, rtl8125a, rtl8125b };
enum class ChipStatus { supported, wrong_device, inaccessible, unknown_mac };

struct ChipIdentity {
    ChipStatus status;
    MacVersion version;
    std::uint16_t xid;
};

// Pure value operation, valid at any IRQL. A supported result identifies a
// source-supported MAC; it does not certify native driver readiness. The caller
// supplies a TxConfig register snapshot from its own validated MMIO mapping.
ChipIdentity identify_chip(std::uint16_t vendor, std::uint16_t device,
                           std::uint32_t tx_config) noexcept;

// Returns an immutable static filename, or nullptr for unsupported versions.
// Firmware bytes and their redistribution permissions are separate inputs.
const char* firmware_name(MacVersion version) noexcept;

} // namespace rtl8125
