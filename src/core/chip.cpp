// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.chip
Purpose: Port the RTL8125 subset of rtl8169_get_mac_version.
Created: 2026-10-24
Architecture: Implements chip.hpp without hardware access.
Upstream copyright: 2002 ShuChen; 2003-2007 Francois Romieu and contributors.
*/
#include "chip.hpp"

namespace rtl8125 {

ChipIdentity identify_chip(std::uint16_t vendor, std::uint16_t device,
                           std::uint32_t tx_config) noexcept
{
    if (vendor != realtek_vendor_id || device != rtl8125_device_id) {
        return {ChipStatus::wrong_device, MacVersion::unsupported, 0};
    }

    if (tx_config == UINT32_MAX) {
        return {ChipStatus::inaccessible, MacVersion::unsupported, 0};
    }

    // Linux rtl_init_one extracts XID before applying the MAC family mask.
    const auto xid = static_cast<std::uint16_t>((tx_config >> 20) & 0xfcfU);
    const auto family = static_cast<std::uint16_t>(xid & 0x7cfU);

    if (family == 0x641U) {
        return {ChipStatus::supported, MacVersion::rtl8125b, xid};
    }

    if (family == 0x609U) {
        return {ChipStatus::supported, MacVersion::rtl8125a, xid};
    }

    return {ChipStatus::unknown_mac, MacVersion::unsupported, xid};
}

const char* firmware_name(MacVersion version) noexcept
{
    switch (version) {
        case MacVersion::rtl8125a:
            return "rtl_nic/rtl8125a-3.fw";
        case MacVersion::rtl8125b:
            return "rtl_nic/rtl8125b-2.fw";
        default:
            return nullptr;
    }
}

} // namespace rtl8125
