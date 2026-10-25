// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.hardware_init
Purpose: Native exclusive checked I/O and fixed-name firmware acquisition.
Created: 2026-10-24
Architecture: Explicit PASSIVE_LEVEL services, unwired to the read-only probe.
*/
#pragma once
#include "resources.hpp"
#include "../hardware/device.hpp"

namespace rtl8125::reactos {

struct HardwareAccess {
    const DeviceResources* resources{};
    volatile LONG claimed{};
    NDIS_HANDLE adapter_handle{};
    UCHAR pci_function{};
};

// The resources mapping and owner outlive all sessions. Interlocked exclusion
// rejects concurrent access; native callbacks enforce PASSIVE_LEVEL. Claim must
// also be serialized by the adapter against resources release and halt.
hardware::RegisterIo hardware_register_io(HardwareAccess&) noexcept;

struct FirmwareBlob {
    PUCHAR bytes{};
    ULONG size{};
    hardware::FirmwareView view{};
    MacVersion version{MacVersion::unsupported};
};

// PASSIVE_LEVEL. Fixed SystemRoot/system32/drivers plus the immutable filename
// selected by core firmware_name; no caller path, device path or network input.
// Opens a kernel synchronous read-only handle, caps size at 1 MiB, reads into a
// private nonpaged copy, validates it, and closes every handle on all paths.
// No firmware is bundled; Realtek firmware terms are separate from driver GPL.
// Blob must be empty. Failure leaves it empty; immutable storage lives to release.
NDIS_STATUS acquire_firmware(MacVersion, FirmwareBlob&) noexcept;

// PASSIVE_LEVEL, after every borrowed view/session user has finished. Idempotent.
void release_firmware(FirmwareBlob&) noexcept;

struct HardwareState {
    hardware::DeviceState device{};
};
// PASSIVE_LEVEL, caller serializes against halt/resource release and packet work.
// Requires adapter_handle and actual PCI function in HardwareAccess, matched
// firmware from acquire_firmware, and empty state. Never called by the probe.
// Failure after the first hardware write attempts independent containment and
// leaves recovery_required set. No DMA allocation can be freed on that basis.
NDIS_STATUS initialize_hardware(HardwareAccess&, const FirmwareBlob&, HardwareState&) noexcept;
// Requires genuine exposed DMA rings and qualified IRQ/recovery platform owner.
// This service never enables IRQs; caller must complete M07/M08 orchestration.
NDIS_STATUS activate_hardware(HardwareAccess&, HardwareState&,
                              const hardware::RingConfiguration&) noexcept;
// PASSIVE_LEVEL, stable mapping/state; errors and down transitions clear output.
NDIS_STATUS query_hardware_link(HardwareAccess&, const HardwareState&,
                                hardware::LinkState&) noexcept;

} // namespace rtl8125::reactos
