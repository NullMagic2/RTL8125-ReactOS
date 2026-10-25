// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.filter
Purpose: Validate filter OIDs and calculate Linux-compatible RTL8125 hash words.
Created: 2026-10-24
Architecture: No allocating containers, globals, OS calls or hardware access.
Derived from Linux rtl_set_rx_mode and ether_crc; see THIRD_PARTY.txt.
*/
#include "filter.hpp"
#include "frame_limits.hpp"

namespace rtl8125 {
namespace {
bool equal_address(const std::uint8_t* left, const std::uint8_t* right) noexcept
{
    for (unsigned index = 0; index < 6; ++index) {
        if (left[index] != right[index]) { return false; }
    }
    return true;
}
bool broadcast(const std::uint8_t* address) noexcept
{
    for (unsigned index = 0; index < 6; ++index) {
        if (address[index] != 0xff) { return false; }
    }
    return true;
}
std::uint32_t ethernet_crc(const std::uint8_t* address) noexcept
{
    std::uint32_t crc = UINT32_MAX;
    for (unsigned byte = 0; byte < 6; ++byte) {
        auto value = address[byte];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const bool carry = ((crc >> 31) ^ value) & 1;
            crc <<= 1;
            if (carry) { crc ^= 0x04c11db7; }
            value >>= 1;
        }
    }
    return crc;
}
std::uint32_t swap_bytes(std::uint32_t value) noexcept
{
    return (value >> 24) | ((value >> 8) & 0x0000ff00) |
        ((value << 8) & 0x00ff0000) | (value << 24);
}
}
InformationResult set_packet_filter(const void* source, std::size_t length,
                                     PacketFilter& filter) noexcept
{
    std::uint32_t value{};
    const auto result = read_information_u32(source, length, value);
    if (result.status != InformationStatus::success) { return result; }
    if ((value & ~supported_packet_filters) != 0) {
        return {InformationStatus::unsupported, 0, 0};
    }
    filter.flags = value;
    return result;
}
InformationResult set_multicast_list(const void* source, std::size_t length,
                                      PacketFilter& filter) noexcept
{
    if (length % 6 != 0 || length / 6 > maximum_multicast_addresses) {
        return {InformationStatus::invalid_length, 0, maximum_multicast_addresses * 6};
    }
    if (source == nullptr && length != 0) { return {InformationStatus::invalid_buffer, 0, 0}; }
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    for (std::size_t index = 0; index < length; index += 6) {
        if ((bytes[index] & 1) == 0 || broadcast(bytes + index)) {
            return {InformationStatus::invalid_value, 0, 0};
        }
    }
    PacketFilter next{};
    next.flags = filter.flags;
    next.multicast_count = length / 6;
    for (std::size_t index = 0; index < length; ++index) {
        next.multicast[index / 6][index % 6] = bytes[index];
    }
    filter = next;
    return {InformationStatus::success, length, 0};
}
FilterRegisters filter_registers(const PacketFilter& filter) noexcept
{
    FilterRegisters result{};
    if ((filter.flags & filter_promiscuous) != 0) {
        return {15, UINT32_MAX, UINT32_MAX};
    }
    if ((filter.flags & filter_directed) != 0) { result.accept |= 2; }
    if ((filter.flags & filter_broadcast) != 0) { result.accept |= 8; }
    if ((filter.flags & filter_all_multicast) != 0) {
        result.accept |= 4;
        result.mar0 = result.mar4 = UINT32_MAX;
    } else if ((filter.flags & filter_multicast) != 0 && filter.multicast_count != 0) {
        result.accept |= 4;
        std::uint32_t words[2]{};
        const auto count = filter.multicast_count <= maximum_multicast_addresses ?
            filter.multicast_count : maximum_multicast_addresses;
        for (std::size_t index = 0; index < count; ++index) {
            const auto bit = ethernet_crc(filter.multicast[index]) >> 26;
            words[bit >> 5] |= UINT32_C(1) << (bit & 31);
        }
        result.mar0 = swap_bytes(words[1]);
        result.mar4 = swap_bytes(words[0]);
    }
    return result;
}
bool accepts_frame(const PacketFilter& filter, const std::uint8_t* station,
                    const std::uint8_t* frame, std::size_t size) noexcept
{
    if (station == nullptr || frame == nullptr || size < ethernet_header_size || size > maximum_frame_size) {
        return false;
    }
    if ((filter.flags & filter_promiscuous) != 0) { return true; }
    if (broadcast(frame)) { return (filter.flags & filter_broadcast) != 0; }
    if ((frame[0] & 1) == 0) {
        return (filter.flags & filter_directed) != 0 && equal_address(station, frame);
    }
    if ((filter.flags & filter_all_multicast) != 0) { return true; }
    if ((filter.flags & filter_multicast) == 0) { return false; }
    const auto count = filter.multicast_count <= maximum_multicast_addresses ?
        filter.multicast_count : maximum_multicast_addresses;
    for (std::size_t index = 0; index < count; ++index) {
        if (equal_address(filter.multicast[index], frame)) { return true; }
    }
    return false;
}
bool valid_station_address(const std::uint8_t* address) noexcept
{
    if (address == nullptr || (address[0] & 1) != 0) { return false; }
    std::uint8_t any{};
    for (unsigned index = 0; index < 6; ++index) { any |= address[index]; }
    return any != 0;
}
} // namespace rtl8125
