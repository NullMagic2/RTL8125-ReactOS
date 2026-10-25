// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.interrupt
Purpose: Translate Linux RTL8125 event semantics into lifetime-tracked NT callbacks.
Created: 2026-10-24
Architecture: Actual NDIS INTx registration; bounded M05/M06 drains, M03 link work.
Adapted from: Linux v6.12 r8169_main.c, rtl8169_interrupt and rtl8169_poll.
*/
#include "adapter.hpp"

extern "C" VOID NTAPI rtl8125_handle_interrupt(NDIS_HANDLE);
namespace rtl8125::reactos {
namespace {
constexpr ULONG mask_register = 0x38, status_register = 0x3c, link_event = 0x20;

bool closing(const InterruptOwner& owner) noexcept
{
    return InterlockedCompareExchange(const_cast<volatile LONG*>(&owner.closing), 0, 0) != 0;
}
void release_reference(InterruptOwner& owner) noexcept
{
    // This is the callback's final owner access. Driver-wide code unloading
    // still requires the independent M08 lifecycle fence.
    InterlockedDecrement(&owner.references);
}
void acquire_reference(InterruptOwner& owner) noexcept
{
    // Scheduling holds the interrupt lock. New references after close are barred.
    InterlockedIncrement(&owner.references);
}
bool mask_and_flush(InterruptOwner& owner) noexcept
{
    owner.masked = true;
    return write_mmio32(owner.adapter->resources, mask_register, 0) &&
        flush_posted_writes(owner.adapter->resources);
}
void fault(InterruptOwner& owner, bool removed = false) noexcept
{
    owner.faulted = true;
    owner.removed = owner.removed || removed;
    owner.enabled = false;
    InterlockedExchange(&owner.closing, 1);
    ++owner.io_failures;
    // No further write after an all-ones removal read. For another I/O failure,
    // make an independent best effort to mask; failure retains all ownership.
    if (!owner.removed) { mask_and_flush(owner); }
}
void queue_dpc(InterruptOwner& owner) noexcept
{
    // Caller owns the interrupt spin lock. A reference precedes publication.
    if (closing(owner) || owner.faulted || !owner.enabled) { return; }
    acquire_reference(owner);
    if (!KeInsertQueueDpc(&owner.continuation, nullptr, nullptr)) {
        release_reference(owner);
    } else {
        ++owner.continuations;
    }
}
bool collect_events(InterruptOwner& owner, bool& recognized) noexcept
{
    recognized = false;
    ULONG status{};
    if (!read_mmio32(owner.adapter->resources, status_register, status)) {
        fault(owner);
        return false;
    }
    // Linux tests the low 16 bits, including devices returning partial all-ones.
    if ((status & 0xffff) == 0xffff) {
        fault(owner, true);
        return false;
    }
    const ULONG events = status & interrupt_event_mask;
    if (events == 0) { ++owner.spurious; return true; }
    recognized = true;
    if (!mask_and_flush(owner) ||
        !write_mmio32(owner.adapter->resources, status_register, events) ||
        !flush_posted_writes(owner.adapter->resources)) {
        fault(owner);
        return false;
    }
    owner.pending |= events;
    if ((events & link_event) != 0) { InterlockedExchange(&owner.link_events, 1); }
    ++owner.recognized;
    return true;
}
BOOLEAN NTAPI queue_synchronized(PVOID context)
{
    auto& owner = *static_cast<InterruptOwner*>(context);
    queue_dpc(owner);
    return TRUE;
}
struct CloseContext { InterruptOwner* owner; bool removed; };
BOOLEAN NTAPI close_synchronized(PVOID context)
{
    auto& close = *static_cast<CloseContext*>(context);
    auto& owner = *close.owner;
    owner.removed = owner.removed || close.removed;
    InterlockedExchange(&owner.closing, 1);
    owner.enabled = false;
    if (!owner.removed && !mask_and_flush(owner)) { fault(owner); }
    return TRUE;
}
struct FinishContext { InterruptOwner* owner; bool more; };
BOOLEAN NTAPI finish_synchronized(PVOID context)
{
    auto& finish = *static_cast<FinishContext*>(context);
    auto& owner = *finish.owner;
    if (closing(owner) || owner.faulted || !owner.enabled) { return FALSE; }
    bool observed{};
    if (!collect_events(owner, observed)) { return FALSE; }
    if (finish.more || observed || (InterlockedCompareExchange(&owner.link_events, 0, 0) != 0 &&
        InterlockedCompareExchange(&owner.link_queued, 0, 0) == 0)) {
        queue_dpc(owner);
        return TRUE;
    }
    // Reading status before and after unmask while synchronized closes the
    // drain/unmask race. A completion after the second read asserts the line.
    if (!write_mmio32(owner.adapter->resources, mask_register, interrupt_event_mask) ||
        !flush_posted_writes(owner.adapter->resources)) { fault(owner); return FALSE; }
    owner.masked = false;
    if (!collect_events(owner, observed)) { return FALSE; }
    if (observed) { queue_dpc(owner); }
    return TRUE;
}
BOOLEAN NTAPI begin_synchronized(PVOID context)
{
    auto& owner = *static_cast<InterruptOwner*>(context);
    if (closing(owner) || !owner.enabled || owner.faulted) { return FALSE; }
    if (!owner.masked && !mask_and_flush(owner)) { fault(owner); return FALSE; }
    owner.pending = 0;
    return TRUE;
}
BOOLEAN NTAPI enable_synchronized(PVOID context)
{
    auto& owner = *static_cast<InterruptOwner*>(context);
    if (closing(owner) || owner.faulted || owner.enabled) { return FALSE; }
    owner.enabled = true;
    // Initial bounded service handles completions generated before IRQ enable.
    InterlockedExchange(&owner.link_events, 1);
    queue_dpc(owner);
    return TRUE;
}
VOID NTAPI deferred(PKDPC, PVOID context, PVOID, PVOID)
{
    auto& owner = *static_cast<InterruptOwner*>(context);
    if (!closing(owner)) { rtl8125_handle_interrupt(owner.adapter); }
    release_reference(owner);
}
struct LinkContext { InterruptOwner* owner; bool schedule; };
BOOLEAN NTAPI reserve_link_synchronized(PVOID context)
{
    auto& link = *static_cast<LinkContext*>(context);
    auto& owner = *link.owner;
    if (closing(owner) || !owner.enabled || owner.faulted ||
        InterlockedCompareExchange(&owner.link_events, 0, 0) == 0 ||
        InterlockedCompareExchange(&owner.link_queued, 1, 0) != 0) { return FALSE; }
    InterlockedExchange(&owner.link_events, 0);
    acquire_reference(owner);
    link.schedule = true;
    return TRUE;
}
void schedule_link(InterruptOwner& owner) noexcept
{
    LinkContext context{&owner, false};
    NdisMSynchronizeWithInterrupt(&owner.interrupt,
        reinterpret_cast<PVOID>(reserve_link_synchronized), &context);
    if (!context.schedule) { return; }
    if (owner.schedule_link == nullptr || !owner.schedule_link(*owner.adapter)) {
        InterlockedExchange(&owner.link_queued, 0);
        InterlockedExchange(&owner.link_events, 1);
        release_reference(owner);
        close_interrupt_admission(*owner.adapter);
    }
}
}

NDIS_STATUS register_interrupt(Adapter& adapter) noexcept
{
    auto& owner = adapter.interrupts;
    const auto& resources = adapter.resources;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || adapter.handle == nullptr ||
        owner.registered || owner.adapter != nullptr || closing(owner) || owner.faulted ||
        resources.mapped_address == nullptr ||
        resources.chip.status != ChipStatus::supported ||
        (resources.chip.version != MacVersion::rtl8125a && resources.chip.version != MacVersion::rtl8125b) ||
        resources.interrupt_vector == 0 ||
        resources.interrupt_level == 0 || resources.interrupt_affinity == 0 ||
        (resources.interrupt_flags & ~CM_RESOURCE_INTERRUPT_LATCHED) != 0) {
        return NDIS_STATUS_INVALID_DATA;
    }
    owner.adapter = &adapter;
    KeInitializeDpc(&owner.continuation, deferred, &owner);
    if (!mask_and_flush(owner)) { owner.adapter = nullptr; return NDIS_STATUS_FAILURE; }
    const auto mode = (resources.interrupt_flags & CM_RESOURCE_INTERRUPT_LATCHED) != 0 ?
        NdisInterruptLatched : NdisInterruptLevelSensitive;
    const auto status = NdisMRegisterInterrupt(&owner.interrupt, adapter.handle,
        resources.interrupt_vector, resources.interrupt_level, TRUE,
        resources.interrupt_shared, mode);
    if (status != NDIS_STATUS_SUCCESS) { owner.adapter = nullptr; return status; }
    owner.registered = true;
    return NDIS_STATUS_SUCCESS;
}
NDIS_STATUS enable_interrupt(Adapter& adapter) noexcept
{
    auto& owner = adapter.interrupts;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !owner.registered || closing(owner) ||
        !adapter.hardware.device.initialized || !adapter.hardware.device.running ||
        adapter.hardware.device.recovery_required || owner.schedule_link == nullptr ||
        adapter.packet_prerequisites.retire_cpu_access == nullptr ||
        adapter.phase != AdapterPhase::running) { return NDIS_STATUS_ADAPTER_NOT_READY; }
    return NdisMSynchronizeWithInterrupt(&owner.interrupt,
        reinterpret_cast<PVOID>(enable_synchronized), &owner) ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE;
}
void close_interrupt_admission(Adapter& adapter, bool removed) noexcept
{
    auto& owner = adapter.interrupts;
    if (!owner.registered || KeGetCurrentIrql() > DISPATCH_LEVEL) { return; }
    CloseContext close{&owner, removed};
    NdisMSynchronizeWithInterrupt(&owner.interrupt,
        reinterpret_cast<PVOID>(close_synchronized), &close);
}
bool retire_interrupt(Adapter& adapter) noexcept
{
    auto& owner = adapter.interrupts;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return false; }
    if (!owner.registered) { return owner.adapter == nullptr; }
    close_interrupt_admission(adapter);
    if (KeRemoveQueueDpc(&owner.continuation)) { release_reference(owner); }
    // Stop has closed every scheduler under the interrupt lock. References were
    // acquired before enqueue, so even not-yet-entered callbacks are represented.
    for (ULONG wait = 0; InterlockedCompareExchange(&owner.references, 0, 0) != 0; ++wait) {
        if (wait == 10000) { return false; }
        NdisMSleep(1000);
    }
    NdisMDeregisterInterrupt(&owner.interrupt);
    owner.registered = false;
    owner.adapter = nullptr;
    return true;
}
void defer_interrupt_service(Adapter& adapter) noexcept
{
    auto& owner = adapter.interrupts;
    if (owner.registered && !closing(owner) && KeGetCurrentIrql() <= DISPATCH_LEVEL) {
        NdisMSynchronizeWithInterrupt(&owner.interrupt,
            reinterpret_cast<PVOID>(queue_synchronized), &owner);
    }
}
bool interrupt_registered(const Adapter& adapter) noexcept
{
    return adapter.interrupts.registered && !closing(adapter.interrupts) && !adapter.interrupts.faulted;
}
void interrupt_isr(Adapter* adapter, PBOOLEAN recognized, PBOOLEAN queue_wrapper) noexcept
{
    if (recognized != nullptr) { *recognized = FALSE; }
    if (queue_wrapper != nullptr) { *queue_wrapper = FALSE; }
    if (adapter == nullptr || recognized == nullptr || queue_wrapper == nullptr) { return; }
    auto& owner = adapter->interrupts;
    if (closing(owner) || !owner.enabled || owner.faulted) { return; }
    bool ours{};
    collect_events(owner, ours);
    *recognized = ours ? TRUE : FALSE;
    if (ours && !owner.faulted) { queue_dpc(owner); }
}
void service_interrupt(Adapter& adapter) noexcept
{
    auto& owner = adapter.interrupts;
    if (!owner.registered || closing(owner) || KeGetCurrentIrql() > DISPATCH_LEVEL) { return; }
    AdapterDispatchGuard guard(&adapter);
    if (!guard) { defer_interrupt_service(adapter); return; }
    if (!NdisMSynchronizeWithInterrupt(&owner.interrupt,
        reinterpret_cast<PVOID>(begin_synchronized), &owner)) { return; }
    if (adapter.phase != AdapterPhase::running || !adapter.hardware.device.running ||
        adapter.hardware.device.recovery_required || !adapter.datapath.running) {
        close_interrupt_admission(adapter);
        return;
    }
    const auto transmitted = poll_transmit(adapter.datapath, interrupt_packet_budget);
    if (closing(owner)) { return; }
    const auto received = poll_receive(adapter.datapath, interrupt_packet_budget);
    if (closing(owner)) { return; }
    if (adapter.datapath.faulted) { close_interrupt_admission(adapter); return; }
    schedule_link(owner);
    FinishContext finish{&owner, transmitted == interrupt_packet_budget || received == interrupt_packet_budget};
    NdisMSynchronizeWithInterrupt(&owner.interrupt,
        reinterpret_cast<PVOID>(finish_synchronized), &finish);
}
void service_interrupt_link(Adapter& adapter) noexcept
{
    auto& owner = adapter.interrupts;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        InterlockedCompareExchange(&owner.link_queued, 2, 1) != 1) { return; }
    if (!closing(owner)) {
        hardware::LinkState link{};
        const auto status = query_hardware_link(adapter.hardware_access, adapter.hardware, link);
        AdapterDispatchGuard guard(&adapter);
        if (guard && !closing(owner) && adapter.phase == AdapterPhase::running &&
            adapter.hardware.device.running && !adapter.hardware.device.recovery_required) {
            const bool was_up = adapter.link_up;
            const auto prior_speed = adapter.link_speed_100bps;
            if (status == NDIS_STATUS_SUCCESS) {
                if (report_packet_link(adapter, link) &&
                    (was_up != adapter.link_up || prior_speed != adapter.link_speed_100bps)) {
                    NdisMIndicateStatus(adapter.handle,
                        adapter.link_up ? NDIS_STATUS_MEDIA_CONNECT : NDIS_STATUS_MEDIA_DISCONNECT,
                        nullptr, 0);
                    NdisMIndicateStatusComplete(adapter.handle);
                }
            } else {
                adapter.link_up = false;
                adapter.link_speed_100bps = 0;
                adapter.hardware.device.recovery_required = true;
                stop_datapath(adapter.datapath);
                close_interrupt_admission(adapter);
            }
        } else if (!closing(owner)) {
            InterlockedExchange(&owner.link_events, 1);
        }
    }
    InterlockedExchange(&owner.link_queued, 0);
    if (!closing(owner)) { defer_interrupt_service(adapter); }
    release_reference(owner);
}
} // namespace rtl8125::reactos
