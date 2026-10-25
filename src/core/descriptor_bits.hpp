// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.descriptor_bits
Purpose: Share exact legacy descriptor masks without host container headers.
Created: 2026-10-24
Architecture: Fixed-width values used by offline encoders and native live rings.
Derived from Linux v6.12 r8169_main.c; see THIRD_PARTY.txt.
*/
#pragma once
#include <cstdint>

namespace rtl8125 {

inline constexpr std::uint32_t descriptor_owned = UINT32_C(1) << 31;
inline constexpr std::uint32_t descriptor_ring_end = UINT32_C(1) << 30;
inline constexpr std::uint32_t descriptor_first = UINT32_C(1) << 29;
inline constexpr std::uint32_t descriptor_last = UINT32_C(1) << 28;
inline constexpr std::uint32_t receive_error = UINT32_C(1) << 21;
inline constexpr std::uint32_t descriptor_length_mask = 0x3fff;

} // namespace rtl8125
