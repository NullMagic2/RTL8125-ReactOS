// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.oid
Purpose: Bound NDIS information transfers without trusting caller alignment.
Created: 2026-10-24
Architecture: Portable byte contracts used directly by native OID callbacks.
*/
#pragma once

#include <cstddef>
#include <cstdint>

namespace rtl8125 {

enum class InformationStatus { success, short_buffer, invalid_buffer, invalid_length,
                               invalid_value, unsupported };

struct InformationResult {
    InformationStatus status;
    std::size_t transferred;
    std::size_t needed;
};

// At most DISPATCH_LEVEL in the miniport. Source and destination must each
// refer to accessible nonoverlapping storage for their stated lengths. A null
// destination is valid for an empty result. Failure never changes output bytes.
InformationResult copy_information(void* destination, std::size_t capacity,
                                   const void* source, std::size_t length) noexcept;

// Return a 64-bit statistic in the x86 NDIS little-endian wire representation.
// Capacity 4..7 requests its low 32 bits, capacity >=8 requests all 64 bits;
// successful replies report needed=8 so legacy callers can discover the width.
// Capacity <4 is a short-buffer error needing 8 bytes. Destination has the same
// validity/lifetime requirements as copy_information; no partial error writes.
InformationResult copy_counter_information(void* destination, std::size_t capacity,
                                           std::uint64_t counter) noexcept;

// Read one little-endian 32-bit OID value from potentially unaligned storage.
// Reject both truncated and oversized values and leave output zero on failure.
InformationResult read_information_u32(const void* source, std::size_t length,
                                       std::uint32_t& value) noexcept;

// This milestone has no active filters or datapath. Permit only filter zero;
// reject every nonzero filter without changing the previously accepted value.
InformationResult set_inactive_packet_filter(const void* source, std::size_t length,
                                            std::uint32_t& filter) noexcept;

// Lookahead describes an upper bound, never a live receive-buffer allocation.
// Values beyond the fixed MTU are invalid and do not change existing state.
InformationResult set_lookahead(const void* source, std::size_t length,
                                std::uint32_t& lookahead) noexcept;

} // namespace rtl8125
