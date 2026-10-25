// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.dma
Purpose: Own NDIS common buffers and live descriptor publication/retirement.
Created: 2026-10-24
Architecture: Explicit callable owner, never enabled by the read-only miniport.
*/
#pragma once
#include <ndis.h>
#include "resources.hpp"
#include "../core/frame_limits.hpp"

namespace rtl8125::reactos {

inline constexpr ULONG dma_ring_count = static_cast<ULONG>(dma_descriptor_count);
inline constexpr ULONG dma_packet_size = static_cast<ULONG>(dma_buffer_size);

enum class DmaStatus { ok, invalid_argument, invalid_state, wrong_irql, allocation_failure,
                       invalid_address, device_owned, not_submitted, retirement_failed };

struct CommonBuffer {
    PVOID virtual_address{};
    NDIS_PHYSICAL_ADDRESS logical_address{};
    ULONG length{};
};

struct LiveDescriptor {
    volatile ULONG options;
    volatile ULONG options2;
    volatile ULONG address_low;
    volatile ULONG address_high;
};
static_assert(sizeof(LiveDescriptor) == legacy_descriptor_size, "RTL8125 legacy DMA descriptor size");
static_assert(alignof(LiveDescriptor) == alignof(ULONG), "Atomic aligned native status accesses");

// Noncopyable per-adapter storage. Construct once after BUS_MASTER attributes;
// NDIS owns the DMA adapter object until miniport teardown, even after buffers
// are released. Caller serializes these APIs against ISR/DPC/timers and halt.
// A failed retirement deliberately retains all four allocations and ownership.
class DmaOwner {
public:
    DmaOwner() noexcept = default;
    DmaOwner(const DmaOwner&) = delete;
    DmaOwner& operator=(const DmaOwner&) = delete;
    const CommonBuffer& tx_ring() const noexcept { return buffers_[0]; }
    const CommonBuffer& rx_ring() const noexcept { return buffers_[1]; }
    bool allocated() const noexcept { return buffers_[0].virtual_address != nullptr; }
    bool device_accessible() const noexcept { return exposed_; }
private:
    friend DmaStatus acquire_dma(NDIS_HANDLE, DmaOwner&) noexcept;
    friend DmaStatus release_dma(DmaOwner&) noexcept;
    friend DmaStatus enable_dma_bus_master(DmaOwner&, const DeviceResources&) noexcept;
    friend DmaStatus expose_dma_rings(DmaOwner&) noexcept;
    friend DmaStatus publish_transmit(DmaOwner&, ULONG, const UCHAR*, ULONG) noexcept;
    friend DmaStatus arm_receive(DmaOwner&, ULONG) noexcept;
    friend DmaStatus read_dma_completion(DmaOwner&, bool, ULONG, ULONG&) noexcept;
    friend DmaStatus copy_received_packet(DmaOwner&, ULONG, UCHAR*, ULONG, ULONG) noexcept;
    friend DmaStatus retire_dma(DmaOwner&, const DeviceResources&,
                               bool (*)(void*) noexcept, void*) noexcept;
    NDIS_HANDLE adapter_{};
    CommonBuffer buffers_[4]{};
    bool tx_pending_[dma_ring_count]{};
    bool rx_pending_[dma_ring_count]{};
    bool adapter_initialized_{};
    bool exposed_{};
    bool retired_{};
    bool retiring_{};
    bool bus_master_verified_{};
};

// PASSIVE_LEVEL. Requires NDIS_ATTRIBUTE_BUS_MASTER already set on this adapter.
// Initializes the real DMA adapter with 32-bit addresses before any allocation.
// Allocates uncached rings and fixed TX/RX arenas; validates the complete returned
// logical range and CPU/bus alignment. Any failure rolls allocations back exactly
// once. Successful adapter setup persists across retries until NDIS teardown.
DmaStatus acquire_dma(NDIS_HANDLE, DmaOwner&) noexcept;

// PASSIVE_LEVEL, same owner/handle. Frees only never-exposed or positively retired
// buffers. Idempotent. Active or uncertain hardware ownership returns an error
// and retains memory; timeout or removal never grants permission to free.
// If PCI BM was enabled before a failed, unexposed start, disables/readbacks BM
// and drains PCIe requests before freeing. Failure retains the owner for retry.
DmaStatus release_dma(DmaOwner&) noexcept;

// PASSIVE_LEVEL after acquire, before any descriptor publication/ring exposure.
// Requires NDIS_ATTRIBUTE_BUS_MASTER established before acquire_dma. Validates
// exact PCI identity, memory-space enable, and writes only the PCI Command word
// to enable bus mastering, preserving unrelated bits; verifies the readback.
// Requires a well-formed PCIe endpoint capability and no pending requests.
// A failed/short write may have changed hardware: caller must retire/retain the
// allocated owner. Successful retirement disables BM; startup must call again.
DmaStatus enable_dma_bus_master(DmaOwner&, const DeviceResources&) noexcept;

// Call BEFORE writing either ring base to hardware, even before enabling engines.
// Publication also marks exposure conservatively. There is no public clear flag.
DmaStatus expose_dma_rings(DmaOwner&) noexcept;

// <= DISPATCH_LEVEL with serialized ring access and stable nonpaged input.
// Input is one complete padded/checksummed 60..1514-byte frame. Copies payload,
// addresses and status, executes a native barrier, then publishes OWN atomically.
// RTL8125 UDP/PTP padding quirks and doorbell remain packet-layer obligations.
DmaStatus publish_transmit(DmaOwner&, ULONG index, const UCHAR*, ULONG size) noexcept;
DmaStatus arm_receive(DmaOwner&, ULONG index) noexcept;

// <= DISPATCH_LEVEL, observes OWN then a native acquire barrier before status.
// Does not confuse an unsubmitted slot with a completed descriptor. Clears the
// software pending flag once; repeated completion returns not_submitted.
DmaStatus read_dma_completion(DmaOwner&, bool transmit, ULONG index, ULONG& options) noexcept;

// <= DISPATCH_LEVEL. Only after RX completion consumed, before rearming. Caller
// supplies a decoded and validated frame length (excluding FCS); validates it
// against the completed descriptor, buffer capacity and frame policy again.
DmaStatus copy_received_packet(DmaOwner&, ULONG index, UCHAR*, ULONG capacity, ULONG frame_size) noexcept;

// PASSIVE_LEVEL, serialized adapter shutdown/recovery. Latches CPU admission
// closed before calling the real M08 fence (M07 IRQ/DPC/work plus packet/reset
// callers). A failed fence retains every owner and rejects later publication.
// On a successful fence performs the full A/B FIFO drain/reset, then disables
// PCI bus mastering and checks PCIe Transactions Pending with bounded waits.
// Transactions Pending covers nonposted requests only: it supplements the
// device FIFO drain/reset and posted MMIO/PCI readbacks, never replaces them.
// Drain/reset failures still attempt PCI containment, but retain all buffers.
// Removal, malformed PCI capability lists and timeouts never grant retirement.
// HardwareState must be invalidated by the caller before the first attempt.
DmaStatus retire_dma(DmaOwner&, const DeviceResources&,
                     bool (*retire_cpu_access)(void*) noexcept, void* context) noexcept;

} // namespace rtl8125::reactos
