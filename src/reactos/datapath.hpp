// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.datapath
Purpose: Callable copied TX, synchronous RX and bounded filter OID ownership.
Created: 2026-10-24
Architecture: Reuses DmaOwner; production probe never starts this module.
*/
#pragma once
#include "dma.hpp"
#include "../core/packet.hpp"
#include "../core/filter.hpp"
#include "../core/frame_limits.hpp"

namespace rtl8125::reactos {
inline constexpr ULONG maximum_packet_buffers = 256;
struct DatapathCounters {
    ULONGLONG tx_submitted{}, tx_completed{}, tx_rejected{}, tx_ring_full{}, tx_aborted{};
    ULONGLONG rx_indicated{}, rx_invalid{}, rx_filtered{}, io_failures{};
};

// Device-owned nonpaged storage. Treat fields as private implementation state;
// the caller constructs once, never copies, and serializes all APIs with its
// ISR/DPC/reset/halt fence. No internal lock supplies that future M07/M08 fence.
// Completion/indication callbacks may synchronously send; reentrant poll/release
// is rejected. Resources and DMA outlive this state and its final release.
struct Datapath final {
    Datapath() noexcept = default;
    Datapath(const Datapath&) = delete;
    Datapath& operator=(const Datapath&) = delete;
    NDIS_HANDLE adapter{};
    DmaOwner* dma{};
    const DeviceResources* resources{};
    NDIS_HANDLE packet_pool{}, buffer_pool{};
    PNDIS_PACKET receive_packet{};
    PNDIS_BUFFER receive_buffer{};
    PNDIS_PACKET pending[dma_ring_count]{};
    ULONG producer{}, consumer{}, outstanding{}, receive_index{};
    UCHAR station[6]{}, transmit_copy[maximum_frame_size]{}, receive_copy[maximum_frame_size]{};
    PacketFilter filter{};
    DatapathCounters counters{};
    bool running{}, faulted{}, blocked{}, sending{}, tx_polling{}, rx_polling{}, buffer_pool_acquired{};
};

// PASSIVE_LEVEL. DMA is already allocated and unexposed, resources identify A/B,
// and station is an actually acquired valid unicast address. Allocates one RX
// packet and MDL once at startup; all failures unwind exactly once. ReactOS
// returns a successful null buffer-pool handle; acquisition is tracked separately.
// Performs no MMIO or descriptor publication. Caller owns state storage.
NDIS_STATUS acquire_datapath(NDIS_HANDLE, DmaOwner&, const DeviceResources&,
                             const UCHAR* station, Datapath&) noexcept;

// PASSIVE_LEVEL after full MAC/PHY setup and before RX engines are enabled.
// Requires external interrupt/recovery fences ready. Programs filters, disables
// RX checksum/VLAN extraction and bounds RxMaxSize, then arms all RX descriptors.
// No ring-base/engine/interrupt setup is implicit. Partial failure retains DMA.
// This callable API is deliberately absent from production Initialize.
NDIS_STATUS start_datapath(Datapath&) noexcept;

// <= DISPATCH_LEVEL, serialized. Rejects future sends and receive polls. This
// changes CPU admission only: it neither stops hardware nor proves DMA retired.
void stop_datapath(Datapath&) noexcept;

// MiniportSend-compatible ownership result at <= DISPATCH_LEVEL. PENDING means
// exactly one later completion, including a doorbell failure after OWN. Every
// other result leaves packet ownership with NDIS. No completion occurs inline.
// Accepts only stack-checksummed non-VLAN packets, bounds chain walks even for
// cyclic or empty chains, and publishes through the existing DMA owner.
NDIS_STATUS send_packet(Datapath&, PNDIS_PACKET) noexcept;

// <= DISPATCH_LEVEL, bounded by min(budget,256). Polling can run while stopped
// to drain genuine completions, but cannot substitute for hardware retirement.
// Callback packet ownership is cleared before invoking NdisMSendComplete.
ULONG poll_transmit(Datapath&, ULONG budget) noexcept;
ULONG poll_receive(Datapath&, ULONG budget) noexcept;

// <= DISPATCH_LEVEL after stop plus positively retired DMA and CPU callbacks.
// Completes every remaining accepted packet once with an error; success/pending
// statuses are invalid. Refuses while DMA can still access the common buffers.
NDIS_STATUS abort_transmits(Datapath&, NDIS_STATUS) noexcept;

// PASSIVE_LEVEL after stop, callback fence, retirement and pending completion.
// Owns only NDIS RX pools/MDL/packet; never releases the separate DMA owner.
NDIS_STATUS release_datapath(Datapath&) noexcept;

// <= DISPATCH_LEVEL with serialization and stable assigned MMIO ownership.
// Query/set cover current filter, multicast list and exact maximum-list size;
// station-address queries are also supported. Set is transactional in software;
// any partial MMIO failure faults admission until controlled recovery. Active
// APIs never advertise VLAN/checksum/jumbo/large-send offloads.
NDIS_STATUS query_datapath_information(const Datapath&, NDIS_OID, PVOID, ULONG,
                                      PULONG written, PULONG needed) noexcept;
NDIS_STATUS set_datapath_information(Datapath&, NDIS_OID, PVOID, ULONG,
                                    PULONG read, PULONG needed) noexcept;
} // namespace rtl8125::reactos
