// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.recovery
Purpose: Serialize actual restart/power work on a system thread joined at Halt.
Created: 2026-10-24
Architecture: Preserves packet ownership until CPU fencing and M04 DMA proof.
Derived lifecycle order from Linux v6.12 rtl8169_down/up; NT owns scheduling.
*/
#include "adapter.hpp"
#include "recovery.hpp"
#include "recovery_cpu.hpp"
#include <new>

namespace rtl8125::reactos {
namespace {
constexpr LONG request_reset_bit = 1, request_link_bit = 2, request_power_bit = 4, request_stop_bit = 8;
constexpr ULONGLONG hang_interval = 50000000; // Five seconds, in 100 ns units.
bool atomic_true(volatile LONG& value) noexcept { return InterlockedCompareExchange(&value, 0, 0) != 0; }
void signal(RecoveryOwner& owner, LONG bit) noexcept
{
    LONG previous{}, desired{};
    do {
        previous = InterlockedCompareExchange(&owner.requests, 0, 0);
        desired = previous | bit;
    } while (InterlockedCompareExchange(&owner.requests, desired, previous) != previous);
    KeSetEvent(&owner.wake, IO_NO_INCREMENT, FALSE);
}
bool registry_lock(DriverRecoveryRegistry& registry) noexcept
{
    for (ULONG attempt = 0; attempt < 10000; ++attempt) {
        if (InterlockedCompareExchange(&registry.lock, 1, 0) == 0) { return true; }
        NdisMSleep(1000);
    }
    return false;
}
bool attach(Adapter& adapter, DriverRecoveryRegistry& registry) noexcept
{
    PDEVICE_OBJECT functional{};
    NdisMGetDeviceProperty(adapter.handle, nullptr, &functional, nullptr, nullptr, nullptr);
    if (functional == nullptr || functional->DriverObject != registry.driver) { return false; }
    if (registry.driver == nullptr || registry.release_adapter == nullptr || atomic_true(registry.closing) ||
        !registry_lock(registry)) { return false; }
    if (atomic_true(registry.closing)) { InterlockedExchange(&registry.lock, 0); return false; }
    auto& owner = adapter.recovery;
    ObReferenceObject(registry.driver);
    // IoDeleteDevice defers final extension destruction while this reference
    // remains. A failed hardware retirement must preserve the NDIS DMA adapter
    // and logical miniport block too, not just our Adapter and common buffers.
    ObReferenceObject(functional);
    owner.functional_device = functional;
    owner.image_reference = true;
    owner.registry = &registry;
    owner.next = registry.first;
    registry.first = &adapter;
    InterlockedExchange(&registry.lock, 0);
    return true;
}
bool detach(Adapter& adapter) noexcept
{
    auto& owner = adapter.recovery;
    auto* registry = owner.registry;
    if (registry == nullptr) { return true; }
    if (!registry_lock(*registry)) { return false; }
    Adapter** position = &registry->first;
    while (*position != nullptr && *position != &adapter) { position = &(*position)->recovery.next; }
    if (*position == &adapter) { *position = owner.next; }
    owner.next = nullptr;
    owner.registry = nullptr;
    const bool referenced = owner.image_reference;
    owner.image_reference = false;
    InterlockedExchange(&registry->lock, 0);
    if (owner.functional_device != nullptr) {
        auto* functional = owner.functional_device; owner.functional_device = nullptr;
        ObDereferenceObject(functional);
    }
    if (referenced) { ObDereferenceObject(registry->driver); }
    return true;
}
bool irq_ready(void* context) noexcept { return interrupt_registered(*static_cast<Adapter*>(context)); }
void defer(void* context) noexcept { defer_interrupt_service(*static_cast<Adapter*>(context)); }
bool activate(void* context, DmaOwner& dma) noexcept
{
    auto& adapter = *static_cast<Adapter*>(context);
    if (expose_dma_rings(dma) != DmaStatus::ok) { return false; }
    hardware::RingConfiguration rings{};
    rings.transmit = static_cast<ULONGLONG>(dma.tx_ring().logical_address.QuadPart);
    rings.receive = static_cast<ULONGLONG>(dma.rx_ring().logical_address.QuadPart);
    return activate_hardware(adapter.hardware_access, adapter.hardware, rings) == NDIS_STATUS_SUCCESS;
}
// CPU ownership retires before M04 performs any hardware/DMA drain or release.
bool stop_owned_path(Adapter& adapter) noexcept
{
    if (!fence_adapter_cpu(&adapter)) { adapter.phase = AdapterPhase::retained; return false; }
    // M04 reset/drain invalidates every previous hardware readiness claim,
    // including a retirement that fails after its first write.
    if (adapter.dma.device_accessible()) {
        adapter.hardware.device.running = false;
        adapter.hardware.device.initialized = false;
        adapter.hardware.device.recovery_required = true;
    }
    return stop_packet_path(adapter) == NDIS_STATUS_SUCCESS;
}
NDIS_STATUS start_owned_path(Adapter& adapter, const PacketFilter& saved) noexcept
{
    auto& owner = adapter.recovery;
    if (atomic_true(owner.removed) || atomic_true(owner.terminate)) { return NDIS_STATUS_ADAPTER_REMOVED; }
    adapter.hardware = {};
    auto status = initialize_hardware(adapter.hardware_access, owner.firmware, adapter.hardware);
    if (status != NDIS_STATUS_SUCCESS) { return status; }
    // Full interrupt retirement owns the only permission to reconstruct this
    // owner. Keep DmaOwner intact: NDIS owns its DMA adapter until teardown.
    adapter.interrupts.~InterruptOwner();
    new (&adapter.interrupts) InterruptOwner{};
    adapter.interrupts.schedule_link = request_link_service;
    status = register_interrupt(adapter);
    if (status != NDIS_STATUS_SUCCESS) { return status; }
    adapter.phase = AdapterPhase::incomplete;
    const PacketPrerequisites prerequisites{&adapter, irq_ready, activate, fence_adapter_cpu, defer};
    status = prepare_packet_path(adapter, prerequisites);
    if (status != NDIS_STATUS_SUCCESS) { return status; }
    adapter.datapath.filter = saved;
    adapter.packet_filter = saved.flags;
    if (enable_dma_bus_master(adapter.dma, adapter.resources) != DmaStatus::ok) { return NDIS_STATUS_FAILURE; }
    status = activate_packet_path(adapter);
    if (status != NDIS_STATUS_SUCCESS) { return status; }
    if (atomic_true(owner.terminate) || atomic_true(owner.removed)) { return NDIS_STATUS_ADAPTER_REMOVED; }
    // State publication is complete before opening external packet admission.
    owner.cpu_fenced = false;
    owner.phase = RecoveryPhase::active;
    owner.last_progress = static_cast<ULONG>(adapter.datapath.counters.tx_completed);
    owner.progress_time = KeQueryInterruptTime();
    InterlockedExchange(&adapter.stop_requested, 0);
    InterlockedExchange(&adapter.dispatch_gate, 0);
    status = enable_interrupt(adapter);
    if (status != NDIS_STATUS_SUCCESS) {
        InterlockedExchange(&adapter.stop_requested, 1);
        close_interrupt_admission(adapter);
        return status;
    }
    return NDIS_STATUS_SUCCESS;
}
// This explicit target-OS compatibility check uses the matching SDK layout.
// The audited wrapper publishes ResetStatus=PENDING only AFTER ResetHandler
// returns. Completing earlier triggers BUGCODE_ID_DRIVER in MiniResetComplete.
bool wrapper_reset_pending(Adapter& adapter) noexcept
{
    auto* block = static_cast<PNDIS_MINIPORT_BLOCK>(adapter.handle);
    KIRQL previous{};
    KeAcquireSpinLock(&block->Lock, &previous);
    const bool pending = block->ResetStatus == NDIS_STATUS_PENDING;
    KeReleaseSpinLock(&block->Lock, previous);
    return pending;
}
bool complete_reset(Adapter& adapter) noexcept
{
    auto& owner = adapter.recovery;
    // A timed worker wake retries pending publication; neither this check nor a
    // timeout grants permission to complete early or abandon an accepted reset.
    if (!owner.reset_result_ready || !wrapper_reset_pending(adapter)) { return false; }
    if (InterlockedCompareExchange(&owner.reset_pending, 0, 1) == 1) {
        owner.completed_generation = owner.generation;
        owner.reset_result_ready = false;
        InterlockedExchange(&owner.operation, 0);
        NdisMResetComplete(adapter.handle, owner.reset_result, FALSE);
    }
    return true;
}
NDIS_STATUS restart(Adapter& adapter) noexcept
{
    if (!fence_adapter_cpu(&adapter)) { adapter.phase = AdapterPhase::retained; return NDIS_STATUS_FAILURE; }
    const auto saved = adapter.datapath.adapter != nullptr ? adapter.datapath.filter : adapter.recovery.saved_filter;
    adapter.recovery.saved_filter = saved;
    if (!stop_owned_path(adapter)) { return NDIS_STATUS_FAILURE; }
    const auto status = start_owned_path(adapter, saved);
    if (status != NDIS_STATUS_SUCCESS) {
        stop_owned_path(adapter);
        adapter.recovery.phase = atomic_true(adapter.recovery.removed) ? RecoveryPhase::removed : RecoveryPhase::failed;
    }
    return status;
}
NDIS_STATUS change_power(Adapter& adapter, NDIS_DEVICE_POWER_STATE state) noexcept
{
    auto& owner = adapter.recovery;
    if (state == NdisDeviceStateD0) {
        const auto status = restart(adapter);
        if (status == NDIS_STATUS_SUCCESS) { owner.current_power = state; }
        return status;
    }
    if (state == owner.current_power) { return NDIS_STATUS_SUCCESS; }
    if (owner.current_power != NdisDeviceStateD0) {
        // Already powered down: the OS may have removed BAR access. Changing
        // among non-D0 states needs no further device register transaction.
        owner.current_power = state;
        return NDIS_STATUS_SUCCESS;
    }
    if (!fence_adapter_cpu(&adapter)) { adapter.phase = AdapterPhase::retained; return NDIS_STATUS_FAILURE; }
    const auto saved = adapter.datapath.adapter != nullptr ? adapter.datapath.filter : owner.saved_filter;
    owner.saved_filter = saved;
    if (!stop_owned_path(adapter)) { return NDIS_STATUS_FAILURE; }
    // Wake is unsupported. PHY BMCR power-down is separate from PCI D-state:
    // NDIS/PnP owns actual PCI power IRPs; never access MMIO after that IRP.
    hardware::RegisterSession io(hardware_register_io(adapter.hardware_access), {256, 10000, 100000});
    std::uint16_t bmcr{};
    if (!io.mdio_write(0x1f, 0) || !io.mdio_read(0, bmcr) || !io.mdio_write(0, bmcr | 0x0800)) {
        owner.phase = RecoveryPhase::failed;
        return NDIS_STATUS_FAILURE;
    }
    // The released datapath preserves desired addressing for the D0 restart.
    adapter.datapath.filter = saved;
    owner.phase = RecoveryPhase::suspended;
    owner.current_power = state;
    return NDIS_STATUS_SUCCESS;
}
VOID NTAPI recovery_thread(PVOID context)
{
    auto& adapter = *static_cast<Adapter*>(context);
    auto& owner = adapter.recovery;
    for (;;) {
        LARGE_INTEGER retry{}; retry.QuadPart = -1000000;
        KeWaitForSingleObject(&owner.wake, Executive, KernelMode, FALSE, &retry);
        const auto requests = InterlockedExchange(&owner.requests, 0);
        if (atomic_true(owner.terminate)) {
            close_interrupt_admission(adapter, atomic_true(owner.removed));
            service_interrupt_link(adapter);
            if (atomic_true(owner.reset_pending)) {
                owner.reset_result = NDIS_STATUS_CLOSING;
                owner.reset_result_ready = true;
                if (!complete_reset(adapter)) { continue; }
            }
            if (InterlockedCompareExchange(&owner.power_pending, 0, 1) == 1) {
                InterlockedExchange(&owner.operation, 0);
                NdisMSetInformationComplete(adapter.handle, NDIS_STATUS_CLOSING);
            }
            if (atomic_true(owner.operation)) { continue; }
            break;
        }
        if ((requests & request_stop_bit) != 0) {
            stop_owned_path(adapter);
            owner.phase = atomic_true(owner.removed) ? RecoveryPhase::removed : RecoveryPhase::failed;
        }
        if ((requests & request_reset_bit) != 0 && atomic_true(owner.reset_pending)) {
            owner.phase = RecoveryPhase::resetting;
            owner.reset_result = restart(adapter);
            owner.reset_result_ready = true;
        }
        if ((requests & request_power_bit) != 0 && atomic_true(owner.power_pending)) {
            const auto state = static_cast<NDIS_DEVICE_POWER_STATE>(owner.pending_power);
            const auto status = atomic_true(owner.removed) ? NDIS_STATUS_ADAPTER_REMOVED : change_power(adapter, state);
            if (InterlockedCompareExchange(&owner.power_pending, 0, 1) == 1) {
                InterlockedExchange(&owner.operation, 0);
                NdisMSetInformationComplete(adapter.handle, status);
            }
        }
        if ((requests & request_link_bit) != 0) { service_interrupt_link(adapter); }
        if (atomic_true(owner.reset_pending) && owner.reset_result_ready) { complete_reset(adapter); }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}
}

bool fence_adapter_cpu(void* context) noexcept
{
    if (context == nullptr || KeGetCurrentIrql() != PASSIVE_LEVEL) { return false; }
    auto& adapter = *static_cast<Adapter*>(context);
    InterlockedExchange(&adapter.stop_requested, 1);
    close_interrupt_admission(adapter, atomic_true(adapter.recovery.removed));
    // Closing gate -1 is permanent until this sole lifecycle owner publishes a
    // fully reconstructed running adapter. Ordinary callbacks cannot reopen it.
    bool fenced{};
    for (ULONG attempt = 0; attempt < 10000; ++attempt) {
        const auto prior = InterlockedCompareExchange(&adapter.dispatch_gate, -1, 0);
        if (prior == 0 || prior == -1) { fenced = true; break; }
        NdisMSleep(1000);
    }
    if (!fenced) { return false; }
    stop_datapath(adapter.datapath);
    service_interrupt_link(adapter);
    if (!retire_interrupt(adapter) || !rendezvous_adapter_processors()) { return false; }
    adapter.recovery.cpu_fenced = true;
    return true;
}
NDIS_STATUS start_recovery(Adapter& adapter, DriverRecoveryRegistry& registry) noexcept
{
    auto& owner = adapter.recovery;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || adapter.handle == nullptr || owner.thread != nullptr ||
        owner.phase != RecoveryPhase::dormant || adapter.resources.mapped_address == nullptr || !attach(adapter, registry)) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }
    owner.phase = RecoveryPhase::starting;
    KeInitializeEvent(&owner.wake, SynchronizationEvent, FALSE);
    auto status = acquire_firmware(adapter.resources.chip.version, owner.firmware);
    if (status != NDIS_STATUS_SUCCESS) { owner.phase = RecoveryPhase::failed; detach(adapter); return status; }
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, nullptr, OBJ_KERNEL_HANDLE, nullptr, nullptr);
    const auto thread_status = PsCreateSystemThread(&owner.thread, THREAD_ALL_ACCESS, &attributes,
        nullptr, nullptr, recovery_thread, &adapter);
    if (!NT_SUCCESS(thread_status)) {
        release_firmware(owner.firmware); owner.phase = RecoveryPhase::failed; detach(adapter); return NDIS_STATUS_RESOURCES;
    }
    const PacketFilter empty{};
    status = start_owned_path(adapter, empty);
    if (status != NDIS_STATUS_SUCCESS) { halt_recovery(adapter); }
    return status;
}
NDIS_STATUS request_reset(Adapter& adapter, PBOOLEAN addressing_reset) noexcept
{
    if (addressing_reset == nullptr) { return NDIS_STATUS_INVALID_DATA; }
    *addressing_reset = FALSE; // We restore every saved filter and lookahead.
    auto& owner = adapter.recovery;
    if (KeGetCurrentIrql() > DISPATCH_LEVEL || owner.thread == nullptr || atomic_true(owner.terminate) ||
        atomic_true(owner.removed) || atomic_true(owner.stopped)) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }
    if (InterlockedCompareExchange(&owner.operation, 1, 0) != 0) { return NDIS_STATUS_RESET_IN_PROGRESS; }
    if (atomic_true(owner.terminate) || atomic_true(owner.stopped) || owner.current_power != NdisDeviceStateD0) {
        InterlockedExchange(&owner.operation, 0); return NDIS_STATUS_ADAPTER_NOT_READY;
    }
    InterlockedExchange(&owner.reset_pending, 1);
    ++owner.generation;
    InterlockedExchange(&adapter.stop_requested, 1);
    close_interrupt_admission(adapter);
    signal(owner, request_reset_bit);
    return NDIS_STATUS_PENDING;
}
BOOLEAN check_for_hang(Adapter& adapter) noexcept
{
    AdapterDispatchGuard guard(&adapter);
    if (!guard || adapter.recovery.thread == nullptr || atomic_true(adapter.stop_requested) ||
        adapter.recovery.phase != RecoveryPhase::active) { return FALSE; }
    if (adapter.hardware.device.recovery_required || adapter.datapath.faulted || adapter.interrupts.faulted) { return TRUE; }
    const auto progress = static_cast<ULONG>(adapter.datapath.counters.tx_completed);
    const auto now = KeQueryInterruptTime();
    auto& owner = adapter.recovery;
    if (!adapter.link_up || adapter.datapath.outstanding == 0 || progress != owner.last_progress) {
        owner.last_progress = progress; owner.progress_time = now; return FALSE;
    }
    return now >= owner.progress_time && now - owner.progress_time >= hang_interval ? TRUE : FALSE;
}
bool request_link_service(Adapter& adapter) noexcept
{
    auto& owner = adapter.recovery;
    if (KeGetCurrentIrql() > DISPATCH_LEVEL || owner.thread == nullptr || atomic_true(owner.terminate)) { return false; }
    signal(owner, request_link_bit);
    return true;
}
NDIS_STATUS request_power(Adapter& adapter, NDIS_DEVICE_POWER_STATE state) noexcept
{
    auto& owner = adapter.recovery;
    if (KeGetCurrentIrql() > DISPATCH_LEVEL || state < NdisDeviceStateD0 || state > NdisDeviceStateD3) { return NDIS_STATUS_INVALID_DATA; }
    if (owner.thread == nullptr || atomic_true(owner.terminate) || atomic_true(owner.removed) || atomic_true(owner.stopped)) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }
    if (InterlockedCompareExchange(&owner.operation, 2, 0) != 0) { return NDIS_STATUS_NOT_ACCEPTED; }
    if (atomic_true(owner.terminate) || atomic_true(owner.stopped)) {
        InterlockedExchange(&owner.operation, 0); return NDIS_STATUS_ADAPTER_NOT_READY;
    }
    InterlockedExchange(&owner.power_pending, 1);
    owner.pending_power = state;
    InterlockedExchange(&adapter.stop_requested, 1);
    close_interrupt_admission(adapter);
    signal(owner, request_power_bit);
    return NDIS_STATUS_PENDING;
}
void request_stop(Adapter& adapter, bool removed) noexcept
{
    InterlockedExchange(&adapter.stop_requested, 1);
    InterlockedExchange(&adapter.recovery.stopped, 1);
    if (removed) { InterlockedExchange(&adapter.recovery.removed, 1); }
    close_interrupt_admission(adapter, removed);
    if (KeGetCurrentIrql() <= DISPATCH_LEVEL && adapter.recovery.thread != nullptr &&
        !atomic_true(adapter.recovery.terminate)) { signal(adapter.recovery, request_stop_bit); }
}
void shutdown_recovery(Adapter& adapter) noexcept
{
    request_stop(adapter);
    if (adapter.recovery.current_power != NdisDeviceStateD0 || atomic_true(adapter.recovery.removed) ||
        adapter.resources.mapped_address == nullptr) { return; }
    // A shutdown has no later memory release. Best effort bounded containment
    // suppresses new interrupts and engines without claiming DMA retirement.
    UCHAR command{};
    if (!write_mmio32(adapter.resources, 0x38, 0) || !flush_posted_writes(adapter.resources) ||
        !read_mmio8(adapter.resources, 0x37, command) || command == 0xff) { return; }
    write_mmio8(adapter.resources, 0x37, static_cast<UCHAR>(command & ~0x0c));
    flush_posted_writes(adapter.resources);
}
bool halt_recovery(Adapter& adapter) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return false; }
    auto& owner = adapter.recovery;
    if (owner.phase == RecoveryPhase::dormant && owner.thread == nullptr && owner.registry == nullptr) {
        return stop_packet_path(adapter) == NDIS_STATUS_SUCCESS && retire_interrupt(adapter);
    }
    request_stop(adapter);
    owner.phase = RecoveryPhase::stopping;
    if (owner.thread != nullptr) {
        InterlockedExchange(&owner.terminate, 1);
        KeSetEvent(&owner.wake, IO_NO_INCREMENT, FALSE);
        if (owner.thread_object == nullptr && !NT_SUCCESS(ObReferenceObjectByHandle(owner.thread,
            SYNCHRONIZE, *PsThreadType, KernelMode, &owner.thread_object, nullptr))) { return false; }
        LARGE_INTEGER timeout{}; timeout.QuadPart = -100000000;
        if (KeWaitForSingleObject(owner.thread_object, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS) { return false; }
        ObDereferenceObject(owner.thread_object); owner.thread_object = nullptr;
        ZwClose(owner.thread); owner.thread = nullptr; owner.joined = true;
    }
    if (!stop_owned_path(adapter)) { owner.phase = RecoveryPhase::failed; return false; }
    release_firmware(owner.firmware);
    owner.phase = RecoveryPhase::stopped;
    return detach(adapter);
}
bool retire_driver_recovery(DriverRecoveryRegistry& registry) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return false; }
    InterlockedExchange(&registry.closing, 1);
    while (registry.first != nullptr) {
        auto* adapter = registry.first;
        if (!halt_recovery(*adapter)) { return false; }
        if (registry.release_adapter == nullptr) { return false; }
        registry.release_adapter(adapter);
    }
    return true;
}
} // namespace rtl8125::reactos
