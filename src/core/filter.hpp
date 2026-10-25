// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.filter
Purpose: Bound NDIS filter/list values and RTL8125 multicast hash images.
Created: 2026-10-24
Architecture: Pure values; checked hardware programming belongs to the datapath.
Derived multicast CRC/register order from Linux rtl_set_rx_mode.
*/
#pragma once
#include "oid.hpp"

namespace rtl8125 {
inline constexpr std::uint32_t filter_directed = 1;
inline constexpr std::uint32_t filter_multicast = 2;
inline constexpr std::uint32_t filter_all_multicast = 4;
inline constexpr std::uint32_t filter_broadcast = 8;
inline constexpr std::uint32_t filter_promiscuous = 0x20;
inline constexpr std::uint32_t supported_packet_filters = 0x2f;
inline constexpr std::size_t maximum_multicast_addresses = 32;
struct PacketFilter {
    std::uint32_t flags{};
    std::size_t multicast_count{};
    std::uint8_t multicast[maximum_multicast_addresses][6]{};
};
struct FilterRegisters { std::uint32_t accept; std::uint32_t mar0; std::uint32_t mar4; };

// Accessible byte storage for length; potentially unaligned. Failure leaves
// state unchanged. Lists contain at most 32 non-broadcast multicast addresses.
InformationResult set_packet_filter(const void*, std::size_t, PacketFilter&) noexcept;
InformationResult set_multicast_list(const void*, std::size_t, PacketFilter&) noexcept;
FilterRegisters filter_registers(const PacketFilter&) noexcept;

// Exact software filtering removes multicast-hash collisions. Frame storage is
// readable through size; station must contain the acquired six-byte MAC.
bool accepts_frame(const PacketFilter&, const std::uint8_t* station,
                   const std::uint8_t* frame, std::size_t size) noexcept;
bool valid_station_address(const std::uint8_t*) noexcept;
} // namespace rtl8125
