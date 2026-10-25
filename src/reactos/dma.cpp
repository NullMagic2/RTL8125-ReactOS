// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.dma
Purpose: Checked real-NDIS common buffers and conservative live DMA ownership.
Created: 2026-10-24
Architecture: Kernel-only; x86 NDIS buffers are uncached, little-endian and coherent.
Derived descriptor ordering from Linux v6.12 r8169_main.c; see THIRD_PARTY.txt.
*/
#include "dma.hpp"
#include "hardware_init.hpp"
#include "../core/frame_limits.hpp"
#include "../core/descriptor_bits.hpp"
#include <new>

namespace rtl8125::reactos {
namespace {
constexpr ULONG ring_bytes = dma_ring_count * sizeof(LiveDescriptor);
constexpr ULONG arena_bytes = dma_ring_count * dma_packet_size;
static_assert(dma_ring_count == 256 && sizeof(LiveDescriptor) == 16, "Pinned legacy ring format");
static_assert(dma_packet_size >= maximum_frame_size + ethernet_fcs_size, "RX arena holds complete frames");

std::uint64_t bus_address(const NDIS_PHYSICAL_ADDRESS& address) noexcept
{
    return (static_cast<std::uint64_t>(static_cast<ULONG>(address.HighPart)) << 32) | address.LowPart;
}

bool valid_buffer(const CommonBuffer& buffer, ULONG alignment) noexcept
{
    const auto address = bus_address(buffer.logical_address);
    return buffer.virtual_address != nullptr && buffer.length != 0 &&
        address <= UINT32_MAX && buffer.length - 1 <= UINT32_MAX - address &&
        buffer.length - 1 <= UINTPTR_MAX - reinterpret_cast<ULONG_PTR>(buffer.virtual_address) &&
        (buffer.logical_address.LowPart & (alignment - 1)) == 0 &&
        (reinterpret_cast<ULONG_PTR>(buffer.virtual_address) & (alignment - 1)) == 0;
}

bool buffers_overlap(const CommonBuffer& left, const CommonBuffer& right) noexcept
{
    const auto left_bus = bus_address(left.logical_address);
    const auto right_bus = bus_address(right.logical_address);
    const auto left_cpu = reinterpret_cast<ULONG_PTR>(left.virtual_address);
    const auto right_cpu = reinterpret_cast<ULONG_PTR>(right.virtual_address);
    return (left_bus <= right_bus + right.length - 1 && right_bus <= left_bus + left.length - 1) ||
        (left_cpu <= right_cpu + right.length - 1 && right_cpu <= left_cpu + left.length - 1);
}

void free_buffer(NDIS_HANDLE adapter, CommonBuffer& buffer) noexcept
{
    if (buffer.virtual_address != nullptr) {
        NdisMFreeSharedMemory(adapter, buffer.length, FALSE, buffer.virtual_address, buffer.logical_address);
    }
    buffer = {};
}

LiveDescriptor* descriptor(const CommonBuffer& ring, ULONG index) noexcept
{
    auto* bytes = static_cast<UCHAR*>(ring.virtual_address) + index * sizeof(LiveDescriptor);
    return static_cast<LiveDescriptor*>(static_cast<void*>(bytes));
}

void initialize_descriptor(LiveDescriptor& desc, ULONG address, ULONG options) noexcept
{
    desc.options = 0;
    desc.options2 = 0;
    desc.address_low = address;
    desc.address_high = 0;
    KeMemoryBarrier();
    desc.options = options;
}

bool pci_header(NDIS_HANDLE adapter, UCHAR (&config)[6]) noexcept
{
    return NdisReadPciSlotInformation(adapter, 0, 0, config, sizeof(config)) == sizeof(config) &&
        config[0] == 0xec && config[1] == 0x10 && config[2] == 0x25 && config[3] == 0x81;
}

bool pci_read(NDIS_HANDLE adapter, ULONG offset, ULONG width, ULONG& result) noexcept
{
    UCHAR bytes[4]{};
    result = 0;
    if (width == 0 || width > 4 || offset > 256 - width ||
        NdisReadPciSlotInformation(adapter, 0, offset, bytes, width) != width) { return false; }
    for (ULONG index = 0; index < width; ++index) { result |= ULONG(bytes[index]) << (index * 8); }
    return true;
}

bool drain_pcie_requests(NDIS_HANDLE adapter) noexcept
{
    ULONG status{}, type{}, pointer{};
    if (!pci_read(adapter, 6, 2, status) || status == 0xffff || (status & 0x10) == 0 ||
        !pci_read(adapter, 0x0e, 1, type) || (type & 0x7f) != 0 ||
        !pci_read(adapter, 0x34, 1, pointer)) { return false; }
    std::uint64_t visited{};
    for (ULONG count = 0; count < 48; ++count) {
        if (pointer < 0x40 || pointer > 0xfc || (pointer & 3) != 0) { return false; }
        const auto bit = UINT64_C(1) << (pointer / 4);
        if ((visited & bit) != 0) { return false; }
        visited |= bit;
        ULONG header{};
        if (!pci_read(adapter, pointer, 2, header) || header == 0xffff) { return false; }
        if ((header & 0xff) == 0x10) {
            ULONG flags{};
            if (pointer > 0xf0 || !pci_read(adapter, pointer + 2, 2, flags) ||
                (flags & 0xf) == 0 || (flags & 0xf) > 2 ||
                ((flags >> 4) & 0xf) > 1) { return false; }
            // PCIe Device Status Transactions Pending concerns nonposted
            // transactions. Posted DMA safety additionally requires successful
            // MAC FIFO/engine drain/reset and PCI/MMIO ordering readbacks.
            for (ULONG attempt = 0; attempt < 100; ++attempt) {
                ULONG device_status{};
                if (!pci_read(adapter, pointer + 0x0a, 2, device_status) || device_status == 0xffff) {
                    return false;
                }
                if ((device_status & 0x20) == 0) { return true; }
                NdisMSleep(1000);
            }
            return false;
        }
        pointer = header >> 8;
    }
    return false;
}

bool disable_bus_master(NDIS_HANDLE adapter) noexcept
{
    UCHAR config[6]{};
    if (!pci_header(adapter, config)) { return false; }
    config[4] &= static_cast<UCHAR>(~4u);
    // Never include the adjacent write-one-to-clear PCI Status register.
    if (NdisWritePciSlotInformation(adapter, 0, 4, config + 4, 2) != 2) { return false; }
    UCHAR verified[6]{};
    return pci_header(adapter, verified) && verified[4] == config[4] && verified[5] == config[5];
}

bool valid_irql() noexcept
{
    return KeGetCurrentIrql() <= DISPATCH_LEVEL;
}
}

DmaStatus acquire_dma(NDIS_HANDLE adapter, DmaOwner& owner) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return DmaStatus::wrong_irql;
    }
    if (adapter == nullptr) {
        return DmaStatus::invalid_argument;
    }
    if (owner.allocated() || owner.exposed_ || (owner.adapter_ != nullptr && owner.adapter_ != adapter)) {
        return DmaStatus::invalid_state;
    }
    if (!owner.adapter_initialized_) {
        if (NdisMInitializeScatterGatherDma(adapter, FALSE, arena_bytes) != NDIS_STATUS_SUCCESS) {
            return DmaStatus::allocation_failure;
        }
        owner.adapter_ = adapter;
        owner.adapter_initialized_ = true;
    }
    owner.retired_ = false;
    owner.retiring_ = false;
    owner.bus_master_verified_ = false;
    DmaStatus result = DmaStatus::ok;
    for (ULONG index = 0; index < 4; ++index) {
        auto& buffer = owner.buffers_[index];
        buffer.length = index < 2 ? ring_bytes : arena_bytes;
        NdisMAllocateSharedMemory(adapter, buffer.length, FALSE,
                                  &buffer.virtual_address, &buffer.logical_address);
        if (buffer.virtual_address == nullptr) {
            result = DmaStatus::allocation_failure;
            break;
        }
        if (!valid_buffer(buffer, index < 2 ? 256 : 64)) {
            result = DmaStatus::invalid_address;
            break;
        }
        for (ULONG previous = 0; previous < index; ++previous) {
            if (buffers_overlap(owner.buffers_[previous], buffer)) { result = DmaStatus::invalid_address; }
        }
        if (result != DmaStatus::ok) { break; }
        NdisZeroMemory(buffer.virtual_address, buffer.length);
    }
    if (result != DmaStatus::ok) {
        for (ULONG remaining = 4; remaining > 0; --remaining) {
            free_buffer(adapter, owner.buffers_[remaining - 1]);
        }
        return result;
    }
    for (ULONG index = 0; index < dma_ring_count; ++index) {
        // Start C++17 object lifetimes explicitly; placement construction has
        // no heap allocation, cookie or C++ runtime dependency.
        new (descriptor(owner.buffers_[0], index)) LiveDescriptor{};
        new (descriptor(owner.buffers_[1], index)) LiveDescriptor{};
        const auto end = index == dma_ring_count - 1 ? descriptor_ring_end : 0;
        initialize_descriptor(*descriptor(owner.buffers_[0], index),
            owner.buffers_[2].logical_address.LowPart + index * dma_packet_size, end);
        initialize_descriptor(*descriptor(owner.buffers_[1], index),
            owner.buffers_[3].logical_address.LowPart + index * dma_packet_size, end | dma_packet_size);
        owner.tx_pending_[index] = false;
        owner.rx_pending_[index] = false;
    }
    return DmaStatus::ok;
}

DmaStatus release_dma(DmaOwner& owner) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return DmaStatus::wrong_irql;
    }
    if (owner.exposed_) {
        return DmaStatus::retirement_failed;
    }
    if (owner.bus_master_verified_ &&
        (!disable_bus_master(owner.adapter_) || !drain_pcie_requests(owner.adapter_))) {
        // Never-published allocations still need cleanup of our PCI enable.
        // Preserve all owners if that cleanup cannot be established.
        owner.exposed_ = true;
        owner.retiring_ = true;
        return DmaStatus::retirement_failed;
    }
    owner.bus_master_verified_ = false;
    for (ULONG remaining = 4; remaining > 0; --remaining) {
        free_buffer(owner.adapter_, owner.buffers_[remaining - 1]);
    }
    for (ULONG index = 0; index < dma_ring_count; ++index) {
        owner.tx_pending_[index] = false;
        owner.rx_pending_[index] = false;
    }
    return DmaStatus::ok;
}

DmaStatus enable_dma_bus_master(DmaOwner& owner, const DeviceResources& resources) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return DmaStatus::wrong_irql; }
    if (!owner.allocated() || owner.exposed_ || owner.retired_ || owner.retiring_ ||
        resources.chip.status != ChipStatus::supported ||
        (resources.chip.version != MacVersion::rtl8125a && resources.chip.version != MacVersion::rtl8125b)) {
        return DmaStatus::invalid_state;
    }
    UCHAR config[6]{}, command{};
    if (!pci_header(owner.adapter_, config) || (config[4] & 2) == 0 ||
        !read_mmio8(resources, 0x37, command) || command == 0xff || (command & 0x1c) != 0 ||
        !drain_pcie_requests(owner.adapter_)) {
        return DmaStatus::invalid_state;
    }
    config[4] |= 4;
    UCHAR verified[6]{};
    if (NdisWritePciSlotInformation(owner.adapter_, 0, 4, config + 4, 2) != 2 ||
        !pci_header(owner.adapter_, verified) || verified[4] != config[4] || verified[5] != config[5] ||
        !flush_posted_writes(resources)) {
        // A short/failed PCI write may still reach hardware. Preserve ownership
        // and force the same stop/drain path as a partially published ring.
        owner.exposed_ = true;
        owner.retiring_ = true;
        return DmaStatus::retirement_failed;
    }
    owner.bus_master_verified_ = true;
    return DmaStatus::ok;
}

DmaStatus expose_dma_rings(DmaOwner& owner) noexcept
{
    if (!valid_irql()) {
        return DmaStatus::wrong_irql;
    }
    if (!owner.allocated() || owner.retired_ || owner.retiring_) {
        return DmaStatus::invalid_state;
    }
    if (!owner.bus_master_verified_) { return DmaStatus::invalid_state; }
    owner.exposed_ = true;
    return DmaStatus::ok;
}

DmaStatus publish_transmit(DmaOwner& owner, ULONG index, const UCHAR* frame, ULONG size) noexcept
{
    if (!valid_irql()) {
        return DmaStatus::wrong_irql;
    }
    if (frame == nullptr || index >= dma_ring_count || size < minimum_tx_size || size > maximum_frame_size) {
        return DmaStatus::invalid_argument;
    }
    if (!owner.allocated() || owner.retired_ || owner.retiring_) {
        return DmaStatus::invalid_state;
    }
    if (!owner.bus_master_verified_) { return DmaStatus::invalid_state; }
    auto& desc = *descriptor(owner.buffers_[0], index);
    if (owner.tx_pending_[index] || (desc.options & descriptor_owned) != 0) {
        return DmaStatus::device_owned;
    }
    auto* packet = static_cast<UCHAR*>(owner.buffers_[2].virtual_address) + index * dma_packet_size;
    NdisMoveMemory(packet, frame, size);
    desc.options2 = 0;
    desc.address_low = owner.buffers_[2].logical_address.LowPart + index * dma_packet_size;
    desc.address_high = 0;
    owner.tx_pending_[index] = true;
    owner.exposed_ = true;
    KeMemoryBarrier();
    desc.options = descriptor_owned | descriptor_first | descriptor_last |
        (index == dma_ring_count - 1 ? descriptor_ring_end : 0) | size;
    return DmaStatus::ok;
}

DmaStatus arm_receive(DmaOwner& owner, ULONG index) noexcept
{
    if (!valid_irql()) {
        return DmaStatus::wrong_irql;
    }
    if (index >= dma_ring_count) {
        return DmaStatus::invalid_argument;
    }
    if (!owner.allocated() || owner.retired_ || owner.retiring_) {
        return DmaStatus::invalid_state;
    }
    if (!owner.bus_master_verified_) { return DmaStatus::invalid_state; }
    auto& desc = *descriptor(owner.buffers_[1], index);
    if (owner.rx_pending_[index] || (desc.options & descriptor_owned) != 0) {
        return DmaStatus::device_owned;
    }
    desc.options2 = 0;
    desc.address_low = owner.buffers_[3].logical_address.LowPart + index * dma_packet_size;
    desc.address_high = 0;
    owner.rx_pending_[index] = true;
    owner.exposed_ = true;
    KeMemoryBarrier();
    desc.options = descriptor_owned | (index == dma_ring_count - 1 ? descriptor_ring_end : 0) | dma_packet_size;
    return DmaStatus::ok;
}

DmaStatus read_dma_completion(DmaOwner& owner, bool transmit, ULONG index, ULONG& options) noexcept
{
    options = 0;
    if (!valid_irql()) {
        return DmaStatus::wrong_irql;
    }
    if (index >= dma_ring_count) {
        return DmaStatus::invalid_argument;
    }
    if (!owner.allocated() || owner.retired_ || owner.retiring_) {
        return DmaStatus::invalid_state;
    }
    auto& pending = transmit ? owner.tx_pending_[index] : owner.rx_pending_[index];
    if (!pending) {
        return DmaStatus::not_submitted;
    }
    const auto& desc = *descriptor(owner.buffers_[transmit ? 0 : 1], index);
    const auto snapshot = desc.options;
    if ((snapshot & descriptor_owned) != 0) {
        return DmaStatus::device_owned;
    }
    KeMemoryBarrier();
    options = snapshot;
    pending = false;
    return DmaStatus::ok;
}

DmaStatus copy_received_packet(DmaOwner& owner, ULONG index, UCHAR* output, ULONG capacity,
                               ULONG frame_size) noexcept
{
    if (!valid_irql()) {
        return DmaStatus::wrong_irql;
    }
    if (index >= dma_ring_count || output == nullptr || frame_size < ethernet_header_size ||
        frame_size > maximum_frame_size || frame_size > capacity) {
        return DmaStatus::invalid_argument;
    }
    if (!owner.allocated() || owner.retired_ || owner.retiring_) {
        return DmaStatus::invalid_state;
    }
    const auto& desc = *descriptor(owner.buffers_[1], index);
    const auto status = desc.options;
    if (owner.rx_pending_[index] || (status & descriptor_owned) != 0) {
        return DmaStatus::device_owned;
    }
    KeMemoryBarrier();
    if ((status & (descriptor_first | descriptor_last)) != (descriptor_first | descriptor_last) ||
        (status & receive_error) != 0 ||
        (status & descriptor_length_mask) != frame_size + ethernet_fcs_size) {
        return DmaStatus::invalid_argument;
    }
    NdisMoveMemory(output, static_cast<UCHAR*>(owner.buffers_[3].virtual_address) +
                   index * dma_packet_size, frame_size);
    return DmaStatus::ok;
}

DmaStatus retire_dma(DmaOwner& owner, const DeviceResources& resources,
                     bool (*retire_cpu_access)(void*) noexcept, void* context) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return DmaStatus::wrong_irql;
    }
    if (!owner.exposed_) {
        return DmaStatus::ok;
    }
    owner.retiring_ = true;
    owner.bus_master_verified_ = false;
    if (retire_cpu_access == nullptr || !retire_cpu_access(context) ||
        resources.chip.status != ChipStatus::supported ||
        (resources.chip.version != MacVersion::rtl8125a && resources.chip.version != MacVersion::rtl8125b)) {
        return DmaStatus::retirement_failed;
    }
    UCHAR config[6]{};
    if (!pci_header(owner.adapter_, config)) {
        return DmaStatus::retirement_failed;
    }
    bool drained{};
    {
        // The real lifecycle fence has joined every hardware accessor. This
        // private session therefore needs no second adapter-global lock.
        HardwareAccess access{};
        access.resources = &resources;
        access.adapter_handle = owner.adapter_;
        hardware::RegisterSession io(hardware_register_io(access));
        drained = (config[4] & 2) != 0 && hardware::quiesce_device(io, resources.chip.version);
    }
    // Always attempt bounded PCI containment after a failed hardware drain.
    // Containment limits further damage but is never permission to free.
    const auto contained = disable_bus_master(owner.adapter_);
    const auto pending_drained = contained && drain_pcie_requests(owner.adapter_);
    const auto posted_flushed = contained && flush_posted_writes(resources);
    if (!drained || !contained || !pending_drained || !posted_flushed) {
        return DmaStatus::retirement_failed;
    }
    KeMemoryBarrier();
    owner.retired_ = true;
    owner.exposed_ = false;
    return DmaStatus::ok;
}

} // namespace rtl8125::reactos
