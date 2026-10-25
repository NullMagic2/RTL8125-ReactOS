// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.frame_limits
Purpose: Own Ethernet frame and fixed DMA arena policy without containers.
Created: 2026-10-24
Architecture: Shared portable/NDIS constants; no descriptors, buffers or OS calls.
*/
#pragma once

#include <cstddef>

namespace rtl8125 {

inline constexpr std::size_t ethernet_header_size = 14;
inline constexpr std::size_t ethernet_fcs_size = 4;
inline constexpr std::size_t minimum_tx_size = 60;
inline constexpr std::size_t maximum_frame_size = 1514;
inline constexpr std::size_t dma_descriptor_count = 256;
inline constexpr std::size_t dma_buffer_size = 2048;
inline constexpr std::size_t legacy_descriptor_size = 16;

} // namespace rtl8125
