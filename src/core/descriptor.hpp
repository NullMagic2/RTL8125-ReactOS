// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.descriptor
Purpose: Encode offline legacy descriptors and validate receive completion values.
Created: 2026-10-24
Architecture: Offline values; native live-ring ownership lives in reactos/dma.
Derived from Linux v6.12 r8169_main.c; see THIRD_PARTY.txt.
*/
#pragma once

#include "frame_limits.hpp"
#include "descriptor_bits.hpp"
#include "receive.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rtl8125 {

// This is an offline byte image, not a volatile or DMA-capable ring element.
// Native integration must publish OWN last with the platform DMA write barrier
// and acquire a completion snapshot after observing OWN clear with a read barrier.
struct DescriptorImage {
    std::array<std::uint8_t, 16> bytes{};
};
static_assert(sizeof(DescriptorImage) == 16, "Legacy descriptor wire size");

struct DmaAddress {
    std::uint64_t value;
};

enum class DescriptorStatus { ready, invalid_ring, invalid_size, address_overflow };
struct PreparedDescriptor {
    DescriptorStatus status;
    DescriptorImage image;
};

// Pure, allocation-free, valid at any IRQL; returns a fully initialized image.
// Caller owns the DMA buffer for the whole hardware lifetime. TX input must
// already be padded, software-checksummed and contain one untagged Ethernet frame.
// The RTL8125 UDP/PTP padding workaround is a future packet-layer obligation.
// Neither function sets OWN or accesses live DMA memory.
PreparedDescriptor prepare_tx(DmaAddress address, std::size_t frame_size,
                              std::size_t index, std::size_t count) noexcept;
PreparedDescriptor prepare_rx(DmaAddress address, std::size_t buffer_size,
                              std::size_t index, std::size_t count) noexcept;


} // namespace rtl8125
