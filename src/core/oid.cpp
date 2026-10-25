// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.oid
Purpose: Implement bounded information-buffer transfers and inactive setters.
Created: 2026-10-24
Architecture: No OS calls, allocation, hardware writes or packet acceptance.
*/
#include "oid.hpp"
#include "frame_limits.hpp"

namespace rtl8125 {

InformationResult copy_information(void* destination, std::size_t capacity,
                                   const void* source, std::size_t length) noexcept
{
    if (capacity < length) {
        return {InformationStatus::short_buffer, 0, length};
    }

    if (length != 0 && (destination == nullptr || source == nullptr)) {
        return {InformationStatus::invalid_buffer, 0, 0};
    }

    auto* output = static_cast<std::uint8_t*>(destination);
    const auto* input = static_cast<const std::uint8_t*>(source);

    for (std::size_t index = 0; index < length; ++index) {
        output[index] = input[index];
    }

    return {InformationStatus::success, length, 0};
}

InformationResult copy_counter_information(void* destination, std::size_t capacity,
                                           std::uint64_t counter) noexcept
{
    constexpr std::size_t counter_width = sizeof(counter);

    if (capacity < sizeof(std::uint32_t)) {
        return {InformationStatus::short_buffer, 0, counter_width};
    }

    std::uint8_t bytes[counter_width]{};

    for (std::size_t index = 0; index < counter_width; ++index) {
        bytes[index] = static_cast<std::uint8_t>(counter >> (index * 8));
    }

    const auto length = capacity < counter_width ? sizeof(std::uint32_t) : counter_width;
    auto result = copy_information(destination, capacity, bytes, length);

    if (result.status == InformationStatus::success) {
        result.needed = counter_width;
    }

    return result;
}

InformationResult read_information_u32(const void* source, std::size_t length,
                                       std::uint32_t& value) noexcept
{
    value = 0;

    if (length != sizeof(value)) {
        return {InformationStatus::invalid_length, 0, sizeof(value)};
    }

    if (source == nullptr) {
        return {InformationStatus::invalid_buffer, 0, 0};
    }

    const auto* bytes = static_cast<const std::uint8_t*>(source);
    value = std::uint32_t{bytes[0]} | (std::uint32_t{bytes[1]} << 8) |
            (std::uint32_t{bytes[2]} << 16) | (std::uint32_t{bytes[3]} << 24);
    return {InformationStatus::success, sizeof(value), 0};
}

InformationResult set_inactive_packet_filter(const void* source, std::size_t length,
                                            std::uint32_t& filter) noexcept
{
    std::uint32_t value{};
    auto result = read_information_u32(source, length, value);

    if (result.status != InformationStatus::success) {
        return result;
    }

    if (value != 0) {
        return {InformationStatus::unsupported, 0, 0};
    }

    filter = 0;
    return result;
}

InformationResult set_lookahead(const void* source, std::size_t length,
                                std::uint32_t& lookahead) noexcept
{
    std::uint32_t value{};
    auto result = read_information_u32(source, length, value);

    if (result.status != InformationStatus::success) {
        return result;
    }

    if (value > maximum_frame_size - ethernet_header_size) {
        return {InformationStatus::invalid_value, 0, 0};
    }

    lookahead = value;
    return result;
}

} // namespace rtl8125
