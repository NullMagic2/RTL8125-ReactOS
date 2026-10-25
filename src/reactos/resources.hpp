// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.resources
Purpose: Own assigned PCI register mappings and checked native register access.
Created: 2026-10-24
Architecture: NDIS boundary; core resources.hpp validates immutable snapshots.
*/
#pragma once

#include <ndis.h>
#include "../core/chip.hpp"

namespace rtl8125::reactos {

struct DeviceResources {
    PUCHAR mapped_address{};
    ULONG mapped_length{};
    NDIS_PHYSICAL_ADDRESS physical_address{};
    ULONG interrupt_vector{};
    ULONG interrupt_level{};
    KAFFINITY interrupt_affinity{};
    USHORT interrupt_flags{};
    BOOLEAN interrupt_shared{};
    UCHAR bar_index{};
    UCHAR pci_revision{};
    ULONG tx_config{};
    ChipIdentity chip{ChipStatus::unknown_mac, MacVersion::unsupported, 0};
};

// PASSIVE_LEVEL, after NdisMSetAttributesEx. Both handles come from the same
// Initialize invocation; configuration_context is WrapperConfigurationContext.
// resources must be zero-initialized and unowned. Acquires assigned BAR mapping
// and performs only a read-only TxConfig probe. Success identifies the chip;
// it never means MAC/PHY/DMA or networking is ready. All failures leave resources
// empty. A second acquisition of an owned mapping is rejected without losing it.
NDIS_STATUS acquire_resources(NDIS_HANDLE adapter_handle,
                              NDIS_HANDLE configuration_context,
                              DeviceResources& resources) noexcept;

// PASSIVE_LEVEL. Idempotent for empty state. Caller must prove no hardware DMA,
// ISR/DPC/timer or other accessor can reference the mapping before release.
// M02 starts none of those activities. Uses the acquiring adapter handle.
void release_resources(NDIS_HANDLE adapter_handle, DeviceResources& resources) noexcept;

// Valid through DIRQL while caller owns a stable mapping and serializes access
// against release. Failed reads clear output; failure never performs device I/O.
// Writes validate the complete access before touching hardware. They do not
// imply posted-write completion; call flush_posted_writes when required by a
// hardware sequence. These functions are not called on the active Linux NIC.
bool read_mmio8(const DeviceResources&, ULONG offset, UCHAR& value) noexcept;
bool read_mmio16(const DeviceResources&, ULONG offset, USHORT& value) noexcept;
bool read_mmio32(const DeviceResources&, ULONG offset, ULONG& value) noexcept;
bool write_mmio8(const DeviceResources&, ULONG offset, UCHAR value) noexcept;
bool write_mmio16(const DeviceResources&, ULONG offset, USHORT value) noexcept;
bool write_mmio32(const DeviceResources&, ULONG offset, ULONG value) noexcept;

// Linux rtl_pci_commit orders posted writes by reading ChipCmd at 0x37.
// False means an invalid mapping or an all-ones read (possible removal).
bool flush_posted_writes(const DeviceResources&) noexcept;

} // namespace rtl8125::reactos
