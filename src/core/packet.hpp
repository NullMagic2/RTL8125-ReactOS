// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.packet
Purpose: Bound frame padding and receive status handling for the copied datapath.
Created: 2026-10-24
Architecture: Allocation-free bytes; native buffer-chain walking belongs to NDIS.
Derived padding rules from Linux v6.12 r8169_main.c; see THIRD_PARTY.txt.
*/
#pragma once
#include <cstddef>
#include <cstdint>

namespace rtl8125 {
enum class PacketStatus { ready, invalid_argument, invalid_length, unsupported_vlan };
struct PreparedPacket { PacketStatus status; std::size_t size; };

// Frame storage is writable and nonpaged through capacity; size bytes are valid.
// Applies RTL8125 minimum/UDP/PTP zero padding without altering existing bytes.
// VLAN insertion/offload is unsupported. IPv4 fragments with nonzero offsets and
// IPv6 extension chains are not parsed as UDP, matching the base-header scope.
// The protocol stack supplies software-completed checksums; no offload requested.
PreparedPacket pad_transmit_frame(std::uint8_t* frame, std::size_t size,
                                  std::size_t capacity) noexcept;

// Reuses the shared descriptor decoder. Zero means OWN/error/fragments/length
// rejection. A nonzero result excludes the FCS and is safe within capacity.
std::size_t received_frame_size(std::uint32_t options, std::size_t capacity) noexcept;
} // namespace rtl8125
