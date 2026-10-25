// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.packet
Purpose: Preserve Linux RTL8125 UDP/PTP padding with bounded packet parsing.
Created: 2026-10-24
Architecture: Portable byte operations, no NDIS, DMA or hardware access.
Derived from Linux rtl8125_quirk_udp_padto; see THIRD_PARTY.txt.
*/
#include "packet.hpp"
#include "receive.hpp"
#include "frame_limits.hpp"

namespace rtl8125 {
namespace {
std::uint16_t network_u16(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}
std::size_t udp_offset(const std::uint8_t* frame, std::size_t size) noexcept
{
    const auto type = network_u16(frame + 12);
    if (type == 0x0800 && size >= ethernet_header_size + 20) {
        const auto* ip = frame + ethernet_header_size;
        const std::size_t header = (ip[0] & 15) * 4;
        if ((ip[0] >> 4) == 4 && header >= 20 && header <= size - ethernet_header_size &&
            ip[9] == 17 && (network_u16(ip + 6) & 0x1fff) == 0) {
            return ethernet_header_size + header;
        }
    } else if (type == 0x86dd && size >= ethernet_header_size + 40 &&
               (frame[14] >> 4) == 6 && frame[20] == 17) {
        return ethernet_header_size + 40;
    }
    return 0;
}
}
PreparedPacket pad_transmit_frame(std::uint8_t* frame, std::size_t size,
                                  std::size_t capacity) noexcept
{
    if (frame == nullptr || size > capacity) {
        return {PacketStatus::invalid_argument, 0};
    }
    if (size < ethernet_header_size || size > maximum_frame_size) {
        return {PacketStatus::invalid_length, 0};
    }
    const auto type = network_u16(frame + 12);
    if (type == 0x8100 || type == 0x88a8) {
        return {PacketStatus::unsupported_vlan, 0};
    }
    auto padded = size < minimum_tx_size ? minimum_tx_size : size;
    const auto offset = size < 128 + 47 ? udp_offset(frame, size) : 0;
    if (offset != 0) {
        const auto transport_size = size - offset;
        // Linux checks offsetof(udphdr, len), which is four, before reading dest.
        if (transport_size >= 4 && transport_size < 47) {
            const auto port = network_u16(frame + offset + 2);
            if (port == 319 || port == 320) {
                const auto target = offset + 47;
                if (target > padded) { padded = target; }
            }
        }
        if (transport_size < 8 && offset + 8 > padded) { padded = offset + 8; }
    }
    if (padded > capacity || padded > maximum_frame_size) {
        return {PacketStatus::invalid_length, 0};
    }
    for (auto index = size; index < padded; ++index) { frame[index] = 0; }
    return {PacketStatus::ready, padded};
}
std::size_t received_frame_size(std::uint32_t options, std::size_t capacity) noexcept
{
    const auto result = decode_receive(options, capacity);
    return result.status == ReceiveStatus::ready ? result.frame_size : 0;
}
} // namespace rtl8125
