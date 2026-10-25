// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.resources
Purpose: Validate PCI config and bounded NT assigned-resource byte snapshots.
Created: 2026-10-24
Architecture: Pure value operations; native resources.cpp owns all OS calls.
*/
#pragma once

#include <cstddef>
#include <cstdint>

namespace rtl8125 {

// Highest direct register used by the pinned RTL8125 sequence is the 32-bit
// EEE_TXIDLE_TIMER_8125 at 0x6048. A legacy 256-byte aperture is insufficient.
inline constexpr std::uint32_t minimum_register_aperture = 0x604c;
inline constexpr std::size_t pci_config_snapshot_size = 64;
inline constexpr std::size_t maximum_resource_list_size = 65536;

struct PciRegisterBar {
    std::uint64_t address{};
    std::uint8_t index{};
    std::uint8_t revision{};
    bool is_64_bit{};
};

struct AssignedResources {
    std::uint64_t address{};
    std::uint32_t length{};
    std::uint32_t interrupt_vector{};
    std::uint32_t interrupt_level{};
    std::uint64_t interrupt_affinity{};
    std::uint16_t interrupt_flags{};
    bool interrupt_shared{};
};

// Validate a complete type-0 Ethernet PCI config snapshot and select the first
// memory BAR, as Linux rtl_init_one does. Never probe BAR size by writing PCI
// config. Output is cleared on failure. Snapshot must be readable for size bytes.
bool decode_register_bar(const std::uint8_t* config, std::size_t size,
                         PciRegisterBar& bar) noexcept;

// Parse NT CM_PARTIAL_RESOURCE_LIST snapshots: 8-byte header, pack-4 descriptor
// stride 16 (i386) or 20 (AMD64). Native static assertions bind these layouts to
// the real SDK. No struct is cast onto raw bytes. Reject unsupported variable
// device-specific data and message interrupts. Select exactly one writable,
// uncached assigned range matching the selected PCI BAR plus one line interrupt.
// Any other memory descriptor overlapping that complete aperture is rejected.
// Assigned physical address must equal config BAR on the targeted x86 platform;
// translated platforms require a future bus-translation contract, not guessing.
// Output is cleared on failure. Callable at any IRQL over valid byte snapshots.
bool decode_assigned_resources(const std::uint8_t* bytes, std::size_t size,
                               std::size_t descriptor_stride,
                               const PciRegisterBar& bar,
                               AssignedResources& resources) noexcept;

// Valid widths are 1/2/4; require natural alignment and containment without
// wrapping offset + width. Does not touch mapped memory or authorize access.
bool valid_mmio_access(std::uint32_t length, std::uint32_t offset,
                       std::uint32_t width) noexcept;

} // namespace rtl8125
