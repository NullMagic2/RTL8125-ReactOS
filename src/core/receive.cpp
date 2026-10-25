// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.receive
Purpose: Validate acquired RX status and FCS bounds before native copying.
Created: 2026-10-24
Architecture: One decoder shared by offline tests and live packet handling.
Derived from Linux v6.12 rtl_rx; see THIRD_PARTY.txt.
*/
#include "receive.hpp"
#include "descriptor_bits.hpp"
#include "frame_limits.hpp"

namespace rtl8125 {
ReceiveCompletion decode_receive(std::uint32_t options,
                                 std::size_t buffer_capacity) noexcept
{
    if ((options & descriptor_owned) != 0) {
        return {ReceiveStatus::hardware_owned, 0};
    }

    if ((options & receive_error) != 0) {
        return {ReceiveStatus::device_error, 0};
    }

    if ((options & (descriptor_first | descriptor_last)) !=
        (descriptor_first | descriptor_last)) {
        return {ReceiveStatus::fragmented, 0};
    }

    const auto wire_size = static_cast<std::size_t>(options & descriptor_length_mask);

    if (wire_size < ethernet_header_size + ethernet_fcs_size ||
        wire_size > buffer_capacity || wire_size > maximum_frame_size + ethernet_fcs_size) {
        return {ReceiveStatus::invalid_size, 0};
    }

    return {ReceiveStatus::ready, wire_size - ethernet_fcs_size};
}

} // namespace rtl8125
