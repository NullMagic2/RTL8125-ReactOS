// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.adapter
Purpose: Own native packet dispatch, hardware identity and retirement boundaries.
Created: 2026-10-24
Architecture: Serialized NDIS 5.1; incomplete prerequisites keep Initialize closed.
*/
#pragma once
#include "resources.hpp"
#include "hardware_init.hpp"
#include "datapath.hpp"
#include "interrupt.hpp"
#include "recovery.hpp"
#include "../core/oid.hpp"

namespace rtl8125::reactos {
enum class AdapterPhase { allocated, probing, incomplete, prepared, running, quiescing, retained, stopping };

// Supplied by the real M07/M08 orchestration owner after hardware initialization.
// Missing callbacks always reject activation. interrupt_ready verifies actual IRQ
// registration/CPU serialization; activate programs this owner's logical rings,
// starts hardware with IRQ masked. It must update HardwareState; the M08 startup
// owner calls enable_interrupt after activate_packet_path returns running and
// before completing Initialize or reopening external admission.
// defer_service queues a later bounded DPC and never invokes it inline.
// retire_cpu_access synchronously revokes IRQ/DPC/timer/restart access, is
// idempotent, and returns true only after the complete CPU fence is established.
// PASSIVE_LEVEL for activate/retire; <= DISPATCH_LEVEL for readiness/defer.
struct PacketPrerequisites {
    void* context{};
    bool (*interrupt_ready)(void*) noexcept{};
    bool (*activate)(void*, DmaOwner&) noexcept{};
    bool (*retire_cpu_access)(void*) noexcept{};
    void (*defer_service)(void*) noexcept{};
};

// Nonpaged placement-constructed storage. Hardware, mapping, DMA and packets have
// one adapter owner. NDIS/caller serialization covers Send/OID/DPC and lifecycle;
// no DESERIALIZE attribute is registered. Read-only Initialize creates none of
// the packet/IRQ state; future prerequisite orchestration calls the APIs below.
struct Adapter {
    NDIS_HANDLE handle{};
    DeviceResources resources{};
    HardwareAccess hardware_access{&resources, 0};
    HardwareState hardware{};
    DmaOwner dma{};
    Datapath datapath{};
    InterruptOwner interrupts{};
    RecoveryOwner recovery{};
    volatile LONG dispatch_gate{}, stop_requested{};
    PacketPrerequisites packet_prerequisites{};
    AdapterPhase phase{AdapterPhase::allocated};
    std::uint32_t packet_filter{};
    std::uint32_t lookahead{maximum_frame_size - ethernet_header_size};
    ULONG link_speed_100bps{};
    bool link_up{}, full_duplex{}, servicing{}, shutdown_registered{};
};

// A short packet callback guard supplies the serialization missing from this
// ReactOS wrapper's HandleDeferredProcessing path. It raises only to DISPATCH,
// never spins or blocks, and rejects concurrent/reentrant admission. Hardware
// PHY transactions remain PASSIVE outside this gate and have their own owner.
class AdapterDispatchGuard final {
public:
    explicit AdapterDispatchGuard(Adapter* adapter) noexcept : adapter_(adapter)
    {
        if (adapter_ == nullptr || KeGetCurrentIrql() > DISPATCH_LEVEL) { adapter_ = nullptr; return; }
        previous_ = KeGetCurrentIrql();
        if (previous_ < DISPATCH_LEVEL) { KeRaiseIrql(DISPATCH_LEVEL, &previous_); raised_ = true; }
        if (InterlockedCompareExchange(&adapter_->dispatch_gate, 1, 0) != 0) {
            adapter_ = nullptr;
            if (raised_) { KeLowerIrql(previous_); raised_ = false; }
            return;
        }
        adapter_->servicing = true;
    }
    ~AdapterDispatchGuard() noexcept
    {
        if (adapter_ != nullptr) {
            adapter_->servicing = false;
            InterlockedExchange(&adapter_->dispatch_gate, 0);
        }
        if (raised_) { KeLowerIrql(previous_); }
    }
    explicit operator bool() const noexcept { return adapter_ != nullptr; }
    AdapterDispatchGuard(const AdapterDispatchGuard&) = delete;
    AdapterDispatchGuard& operator=(const AdapterDispatchGuard&) = delete;
private:
    Adapter* adapter_{};
    KIRQL previous_{};
    bool raised_{};
};

// PASSIVE_LEVEL, assigned resources and actual initialized/stopped M03 hardware,
// BUS_MASTER attributes already owned by the prerequisite initializer. Uses the
// actual permanent address and requires real IRQ/recovery callbacks. Allocates
// the adapter's sole DMA/packet owners; no hardware activation is implicit.
NDIS_STATUS prepare_packet_path(Adapter&, const PacketPrerequisites&) noexcept;

// PASSIVE_LEVEL, same actual prerequisites. Arms RX then invokes the platform
// activation boundary. Admission opens only if real HardwareState is running.
// Any partial failure stops admission and attempts positively fenced retirement.
NDIS_STATUS activate_packet_path(Adapter&) noexcept;

// PASSIVE_LEVEL. Stops admission, fences callbacks, proves DMA retirement, then
// aborts pending sends and releases pools/common buffers. Any uncertainty retains
// every live owner and mapping; caller must retain Adapter storage too. Repeated
// calls permit an actual recovery owner to retry a failed fence/retirement.
NDIS_STATUS stop_packet_path(Adapter&) noexcept;

// <= DISPATCH_LEVEL, serialized, result from actual M03 PHY query. Invalid link
// reports are rejected without inventing speed. No status notification is emitted
// here; M03/M07 orchestration owns link-event notification and scheduling.
bool report_packet_link(Adapter&, const hardware::LinkState&) noexcept;
} // namespace rtl8125::reactos
