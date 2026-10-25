// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.interrupt
Purpose: Assigned INTx ownership, bounded private DPC scheduling and CPU retirement.
Created: 2026-10-24
Architecture: M07; Initialize remains closed until the M08 recovery owner exists.
*/
#pragma once
#include "resources.hpp"

namespace rtl8125::reactos {
struct Adapter;
inline constexpr ULONG interrupt_event_mask = 0x2f;
inline constexpr ULONG interrupt_packet_budget = 64;

// Nonpaged, noncopyable, adapter-owned storage. All normal IRQ state below is
// protected by the registered interrupt spin lock. Atomic fields admit DIRQL,
// DPC and PASSIVE teardown without relying on ReactOS wrapper serialization.
// The real M08 owner installs schedule_link to signal its joined passive
// thread. It must eventually call service_interrupt_link exactly once for each
// accepted request, including cancellation after close; false releases the ref.
// Private DPCs are reference tracked; the ISR never queues the wrapper DPC,
// because this ReactOS wrapper does not drain it on deregistration.
struct InterruptOwner final {
    InterruptOwner() noexcept = default;
    InterruptOwner(const InterruptOwner&) = delete;
    InterruptOwner& operator=(const InterruptOwner&) = delete;
    NDIS_MINIPORT_INTERRUPT interrupt{};
    KDPC continuation{};
    bool (*schedule_link)(Adapter&) noexcept{};
    Adapter* adapter{};
    volatile LONG references{}, closing{}, link_queued{}, link_events{};
    ULONG pending{}, recognized{}, spurious{}, continuations{}, io_failures{};
    bool registered{}, enabled{}, masked{true}, faulted{}, removed{};
};

// PASSIVE_LEVEL. Requires an assigned supported A/B mapping and an empty owner.
// Masks and flushes before registering actual assigned line metadata. Message
// interrupts/invalid modes are rejected; routing and MSI feasibility remain
// independent platform qualification. Performs no engine/DMA activation.
NDIS_STATUS register_interrupt(Adapter&) noexcept;

// PASSIVE_LEVEL after actual MAC/DMA activation; requires real running device,
// no recovery fault and a real joined-thread M08 link scheduler. Arms its assigned
// line, schedules initial packet/link service and closes any enable race.
NDIS_STATUS enable_interrupt(Adapter&) noexcept;

// <= DISPATCH_LEVEL. Close scheduling and mask at synchronized DIRQL. This is
// CPU admission only, not DMA retirement. No allocation, waiting or reset.
void close_interrupt_admission(Adapter&, bool removed = false) noexcept;

// PASSIVE_LEVEL, serialized against registration, enable and all lifecycle
// owners. Closes IRQ admission, removes a queued private DPC, waits boundedly for all
// tracked DPC/link references, then deregisters the actual interrupt. Idempotent.
// A false return retains every owner and BAR; no timeout proves DMA stopped.
// When called from the M08 link/recovery thread, close then call
// service_interrupt_link first to cancel its own queued link request.
// This fence covers only M07 callbacks; M08 must also fence timers/recovery and
// ordinary packet callers before using it as retire_cpu_access.
bool retire_interrupt(Adapter&) noexcept;

// <= DISPATCH_LEVEL, non-inline bounded service scheduling. Requires registered
// state owned by the adapter. Stops silently after close/removal/fault.
void defer_interrupt_service(Adapter&) noexcept;
// Query used by the M08 orchestrator; registration is real, never a ready flag.
bool interrupt_registered(const Adapter&) noexcept;

// NDIS callback entry points. ISR is DIRQL; HandleInterrupt is <= DISPATCH_LEVEL.
// Outputs are initialized even for null contexts. ISR reads status once, masks,
// flushes, acknowledges only observed known events, then queues the private DPC.
void interrupt_isr(Adapter*, PBOOLEAN recognized, PBOOLEAN queue_wrapper) noexcept;
void service_interrupt(Adapter&) noexcept;

// PASSIVE_LEVEL, called exclusively by the actual M08 joined worker. Claims one
// queued link request; closes/invalid IRQL never invent a PHY result. After close
// a pending request is cancelled and its ref released without any hardware I/O.
// Caller must join the thread for driver-code unload, beyond this object fence.
void service_interrupt_link(Adapter&) noexcept;
} // namespace rtl8125::reactos
