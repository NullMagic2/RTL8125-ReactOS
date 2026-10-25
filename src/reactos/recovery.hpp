// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.recovery
Purpose: Joinable passive recovery, generation ownership and whole-adapter fencing.
Created: 2026-10-24
Architecture: M08 owns lifecycle; M04 owns DMA retirement and M07 owns assigned IRQs.
*/
#pragma once
#include "hardware_init.hpp"
#include "../core/filter.hpp"

namespace rtl8125::reactos {
struct Adapter;
struct DriverRecoveryRegistry {
    PDRIVER_OBJECT driver{};
    void (*release_adapter)(Adapter*) noexcept{};
    Adapter* first{};
    volatile LONG lock{}, closing{};
};
enum class RecoveryPhase { dormant, starting, active, resetting, suspended, failed, removed, stopping, stopped };
struct RecoveryOwner final {
    HANDLE thread{};
    PVOID thread_object{};
    KEVENT wake{};
    DriverRecoveryRegistry* registry{};
    PDEVICE_OBJECT functional_device{};
    Adapter* next{};
    FirmwareBlob firmware{};
    PacketFilter saved_filter{};
    volatile LONG requests{}, terminate{}, reset_pending{}, power_pending{}, removed{}, operation{}, stopped{};
    NDIS_STATUS reset_result{NDIS_STATUS_FAILURE};
    bool reset_result_ready{};
    ULONG generation{}, completed_generation{}, last_progress{}, pending_power{NdisDeviceStateD0};
    ULONGLONG progress_time{};
    RecoveryPhase phase{RecoveryPhase::dormant};
    bool joined{}, image_reference{}, cpu_fenced{};
    NDIS_DEVICE_POWER_STATE current_power{NdisDeviceStateD0};
};

// PASSIVE, actual mapped A/B adapter and BUS_MASTER NDIS attributes required.
// Creates the sole joinable passive thread and firmware owner. Performs actual
// MAC/PHY, assigned IRQ and DMA startup; failure runs the same retirement path.
// The default probe leaves this inactive; the explicit operational build calls
// it only after actual FDO/PDO identity and the target-wrapper patchset are set.
NDIS_STATUS start_recovery(Adapter&, DriverRecoveryRegistry&) noexcept;
// <= DISPATCH, short nonblocking requests. Reset returns PENDING only once;
// the joined thread completes it exactly once after wrapper pending publication.
NDIS_STATUS request_reset(Adapter&, PBOOLEAN addressing_reset) noexcept;
BOOLEAN check_for_hang(Adapter&) noexcept;
bool request_link_service(Adapter&) noexcept;
// <= DISPATCH. Copies scalar power state; completion is asynchronous and owned
// by the sole worker. Query requires D0..D3 and rejects wake capabilities.
NDIS_STATUS request_power(Adapter&, NDIS_DEVICE_POWER_STATE) noexcept;
void request_stop(Adapter&, bool removed = false) noexcept;
// Shutdown may run above DISPATCH: bounded register containment only there;
// no worker queue, wait, allocation or memory release is performed at high IRQL.
void shutdown_recovery(Adapter&) noexcept;
// PASSIVE. Close request admission, join the actual system thread, fence packet
// callers/IRQ/DPC/link work, retire DMA then release firmware. False retains the
// adapter, mapping and driver-image reference. Never call from its own thread.
bool halt_recovery(Adapter&) noexcept;
// Real PacketPrerequisites CPU callback, PASSIVE. Closes ordinary admission via
// a persistent gate and retires M07 callbacks. Sole worker may call it on itself;
// halt joins that worker first. No timeout is retirement proof.
bool fence_adapter_cpu(void*) noexcept;
// PASSIVE unload after normal NDIS Halt. Retries retained owners, releases their
// driver references only after joined threads and complete CPU/DMA retirement.
bool retire_driver_recovery(DriverRecoveryRegistry&) noexcept;
} // namespace rtl8125::reactos
