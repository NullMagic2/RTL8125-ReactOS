// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.receive
Purpose: Share receive status validation without offline image containers.
Created: 2026-10-24
Architecture: Container-free common contract used by native copied RX.
Derived from Linux v6.12 rtl_rx; see THIRD_PARTY.txt.
*/
#pragma once
#include <cstddef>
#include <cstdint>

namespace rtl8125 {
enum class ReceiveStatus { ready, hardware_owned, device_error, fragmented, invalid_size };
struct ReceiveCompletion {
    ReceiveStatus status;
    std::size_t frame_size;
};

// Consumes a host-endian status snapshot acquired by the native DMA owner.
// Validates length against actual buffer capacity before subtracting the FCS.
// Returns no borrowed pointers; all error results carry frame_size zero.
ReceiveCompletion decode_receive(std::uint32_t options,
                                 std::size_t buffer_capacity) noexcept;

} // namespace rtl8125
