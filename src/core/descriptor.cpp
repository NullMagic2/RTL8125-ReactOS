// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.descriptor
Purpose: Port bounded legacy TX/RX descriptor value handling from Linux r8169.
Created: 2026-10-24
Architecture: Implements descriptor.hpp; no MMIO, barriers or ring allocation.
Upstream copyright: 2002 ShuChen; 2003-2007 Francois Romieu and contributors.
*/
#include "descriptor.hpp"

#include <limits>

namespace rtl8125 {
namespace {

PreparedDescriptor prepare(DmaAddress address, std::size_t size,
                           std::size_t index, std::size_t count,
                           std::uint32_t options) noexcept
{
    if (count == 0 || index >= count) {
        return {DescriptorStatus::invalid_ring, {}};
    }

    if (size == 0 || size > descriptor_length_mask) {
        return {DescriptorStatus::invalid_size, {}};
    }

    if (address.value > std::numeric_limits<std::uint64_t>::max() - (size - 1)) {
        return {DescriptorStatus::address_overflow, {}};
    }

    options |= static_cast<std::uint32_t>(size);

    if (index == count - 1) {
        options |= descriptor_ring_end;
    }

    DescriptorImage result{};

    for (std::size_t byte = 0; byte < 4; ++byte) {
        result.bytes[byte] = static_cast<std::uint8_t>(options >> (byte * 8));
    }

    for (std::size_t byte = 0; byte < 8; ++byte) {
        result.bytes[byte + 8] = static_cast<std::uint8_t>(address.value >> (byte * 8));
    }

    return {DescriptorStatus::ready, result};
}

} // namespace

PreparedDescriptor prepare_tx(DmaAddress address, std::size_t frame_size,
                              std::size_t index, std::size_t count) noexcept
{
    if (frame_size < minimum_tx_size || frame_size > maximum_frame_size) {
        return {DescriptorStatus::invalid_size, {}};
    }

    return prepare(address, frame_size, index, count, descriptor_first | descriptor_last);
}

PreparedDescriptor prepare_rx(DmaAddress address, std::size_t buffer_size,
                              std::size_t index, std::size_t count) noexcept
{
    if (buffer_size < maximum_frame_size + ethernet_fcs_size) {
        return {DescriptorStatus::invalid_size, {}};
    }

    return prepare(address, buffer_size, index, count, 0);
}


} // namespace rtl8125
