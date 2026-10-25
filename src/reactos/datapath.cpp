// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.datapath
Purpose: Translate bounded packet values into genuine NDIS/DMA ownership.
Created: 2026-10-24
Architecture: Callable and unwired; exclusive adapter serialization is required.
Doorbell/filter ordering derives from Linux v6.12 r8169_main.c.
*/
#include "datapath.hpp"

namespace rtl8125::reactos {
namespace {
bool callback_active(const Datapath& path) noexcept
{
    return path.sending || path.tx_polling || path.rx_polling;
}
bool valid_dispatch() noexcept { return KeGetCurrentIrql() <= DISPATCH_LEVEL; }
void fault(Datapath& path) noexcept
{
    path.faulted = true;
    path.running = false;
    ++path.counters.io_failures;
}
bool program_filter(Datapath& path, const PacketFilter& filter) noexcept
{
    const auto words = filter_registers(filter);
    ULONG config{};
    if (!read_mmio32(*path.resources, 0x44, config) || config == MAXULONG ||
        !write_mmio32(*path.resources, 0x0c, words.mar4) ||
        !write_mmio32(*path.resources, 0x08, words.mar0) ||
        !write_mmio32(*path.resources, 0x44,
            (config & ~static_cast<ULONG>(0x00c0003f)) | words.accept) ||
        !flush_posted_writes(*path.resources)) {
        fault(path);
        return false;
    }
    return true;
}
void free_receive(Datapath& path) noexcept
{
    if (path.receive_packet != nullptr) {
        NdisReinitializePacket(path.receive_packet);
    }
    if (path.receive_buffer != nullptr) { NdisFreeBuffer(path.receive_buffer); }
    path.receive_buffer = nullptr;
    if (path.receive_packet != nullptr) { NdisFreePacket(path.receive_packet); }
    path.receive_packet = nullptr;
    if (path.buffer_pool_acquired) { NdisFreeBufferPool(path.buffer_pool); }
    path.buffer_pool_acquired = false;
    path.buffer_pool = nullptr;
    if (path.packet_pool != nullptr) { NdisFreePacketPool(path.packet_pool); }
    path.packet_pool = nullptr;
}
NDIS_STATUS gather_packet(PNDIS_PACKET packet, UCHAR* output, ULONG& length) noexcept
{
    length = 0;
    if (packet == nullptr) { return NDIS_STATUS_INVALID_PACKET; }
    // NDIS packet/extension storage comes from NDIS, never a raw user pointer.
    // The stack must finish software checksums and segmentation before Send.
    if (NDIS_PER_PACKET_INFO_FROM_PACKET(packet, TcpIpChecksumPacketInfo) != nullptr ||
        NDIS_PER_PACKET_INFO_FROM_PACKET(packet, TcpLargeSendPacketInfo) != nullptr ||
        NDIS_PER_PACKET_INFO_FROM_PACKET(packet, Ieee8021QInfo) != nullptr) {
        return NDIS_STATUS_NOT_SUPPORTED;
    }
    PNDIS_BUFFER buffer{};
    // Request ONLY FirstBuffer: TotalPacketLength would walk a cyclic chain
    // inside the NDIS macro before our own bounded validation could run.
    NdisQueryPacket(packet, nullptr, nullptr, &buffer, nullptr);
    ULONG buffers{};
    while (buffer != nullptr) {
        if (++buffers > maximum_packet_buffers) { return NDIS_STATUS_INVALID_PACKET; }
        UINT size{};
        PVOID address{};
        NdisQueryBufferSafe(buffer, &address, &size, NormalPagePriority);
        if (size > maximum_frame_size - length) { return NDIS_STATUS_INVALID_LENGTH; }
        if (size != 0) {
            if (address == nullptr) { return NDIS_STATUS_RESOURCES; }
            NdisMoveMemory(output + length, address, size);
            length += size;
        }
        NdisGetNextBuffer(buffer, &buffer);
    }
    const auto prepared = pad_transmit_frame(output, length, maximum_frame_size);
    if (prepared.status == PacketStatus::unsupported_vlan) { return NDIS_STATUS_NOT_SUPPORTED; }
    if (prepared.status != PacketStatus::ready) { return NDIS_STATUS_INVALID_LENGTH; }
    length = static_cast<ULONG>(prepared.size);
    return NDIS_STATUS_SUCCESS;
}
NDIS_STATUS information_status(InformationStatus status) noexcept
{
    switch (status) {
    case InformationStatus::success: return NDIS_STATUS_SUCCESS;
    case InformationStatus::short_buffer: return NDIS_STATUS_BUFFER_TOO_SHORT;
    case InformationStatus::invalid_length: return NDIS_STATUS_INVALID_LENGTH;
    case InformationStatus::unsupported: return NDIS_STATUS_NOT_SUPPORTED;
    default: return NDIS_STATUS_INVALID_DATA;
    }
}
}
NDIS_STATUS acquire_datapath(NDIS_HANDLE adapter, DmaOwner& dma,
                             const DeviceResources& resources, const UCHAR* station,
                             Datapath& path) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return NDIS_STATUS_FAILURE; }
    if (adapter == nullptr || !dma.allocated() || dma.device_accessible() ||
        resources.chip.status != ChipStatus::supported || resources.mapped_address == nullptr ||
        resources.mapped_length < 0xe2 || !valid_station_address(station) ||
        path.adapter != nullptr || callback_active(path)) { return NDIS_STATUS_INVALID_DATA; }
    path.adapter = adapter;
    path.dma = &dma;
    path.resources = &resources;
    NDIS_STATUS status{};
    NdisAllocatePacketPool(&status, &path.packet_pool, 1, 0);
    if (status == NDIS_STATUS_SUCCESS && path.packet_pool != nullptr) {
        NdisAllocateBufferPool(&status, &path.buffer_pool, 1);
        path.buffer_pool_acquired = status == NDIS_STATUS_SUCCESS;
    }
    // ReactOS buffer.c implements buffer pools as successful null handles.
    if (status == NDIS_STATUS_SUCCESS && path.buffer_pool_acquired) {
        NdisAllocatePacket(&status, &path.receive_packet, path.packet_pool);
    }
    if (status == NDIS_STATUS_SUCCESS && path.receive_packet != nullptr) {
        NdisAllocateBuffer(&status, &path.receive_buffer, path.buffer_pool,
                           path.receive_copy, sizeof(path.receive_copy));
    }
    if (status != NDIS_STATUS_SUCCESS || path.receive_buffer == nullptr) {
        free_receive(path);
        path.adapter = nullptr;
        path.dma = nullptr;
        path.resources = nullptr;
        return NDIS_STATUS_RESOURCES;
    }
    for (unsigned index = 0; index < 6; ++index) { path.station[index] = station[index]; }
    NdisReinitializePacket(path.receive_packet);
    NdisChainBufferAtBack(path.receive_packet, path.receive_buffer);
    NDIS_SET_PACKET_HEADER_SIZE(path.receive_packet, ethernet_header_size);
    NDIS_SET_PACKET_STATUS(path.receive_packet, NDIS_STATUS_RESOURCES);
    return NDIS_STATUS_SUCCESS;
}
NDIS_STATUS start_datapath(Datapath& path) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || path.adapter == nullptr || path.running ||
        path.faulted || callback_active(path) || path.outstanding != 0 ||
        !path.dma->allocated() || path.dma->device_accessible()) { return NDIS_STATUS_FAILURE; }
    USHORT command{};
    if (!program_filter(path, path.filter) || !read_mmio16(*path.resources, 0xe0, command) ||
        command == 0xffff ||
        !write_mmio16(*path.resources, 0xe0, static_cast<USHORT>(command & ~0x20u)) ||
        !write_mmio16(*path.resources, 0xda, maximum_frame_size + ethernet_fcs_size + 1) ||
        !flush_posted_writes(*path.resources)) {
        if (!path.faulted) { fault(path); }
        return NDIS_STATUS_FAILURE;
    }
    for (ULONG index = 0; index < dma_ring_count; ++index) {
        if (arm_receive(*path.dma, index) != DmaStatus::ok) {
            fault(path);
            return NDIS_STATUS_FAILURE;
        }
    }
    path.running = true;
    return NDIS_STATUS_SUCCESS;
}
void stop_datapath(Datapath& path) noexcept { path.running = false; }
NDIS_STATUS send_packet(Datapath& path, PNDIS_PACKET packet) noexcept
{
    if (!valid_dispatch() || path.adapter == nullptr || !path.running || path.faulted) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }
    if (path.sending) { return NDIS_STATUS_RESOURCES; }
    if (path.outstanding == dma_ring_count) {
        path.blocked = true;
        ++path.counters.tx_ring_full;
        return NDIS_STATUS_RESOURCES;
    }
    // Refuse a duplicate packet pointer without creating a second completion.
    for (const auto* pending : path.pending) {
        if (pending != nullptr && pending == packet) { return NDIS_STATUS_INVALID_PACKET; }
    }
    path.sending = true;
    ULONG length{};
    const auto status = gather_packet(packet, path.transmit_copy, length);
    if (status != NDIS_STATUS_SUCCESS) {
        path.sending = false;
        ++path.counters.tx_rejected;
        return status;
    }
    const auto index = path.producer;
    if (path.pending[index] != nullptr ||
        publish_transmit(*path.dma, index, path.transmit_copy, length) != DmaStatus::ok) {
        path.sending = false;
        fault(path);
        return NDIS_STATUS_FAILURE;
    }
    path.pending[index] = packet;
    path.producer = (index + 1) % dma_ring_count;
    ++path.outstanding;
    ++path.counters.tx_submitted;
    // OWN publication cannot be rolled back if this checked doorbell fails.
    // Keep NDIS packet ownership pending until completion or proven retirement.
    if (!write_mmio16(*path.resources, 0x90, 1)) { fault(path); }
    path.sending = false;
    return NDIS_STATUS_PENDING;
}
ULONG poll_transmit(Datapath& path, ULONG budget) noexcept
{
    if (!valid_dispatch() || path.adapter == nullptr || path.tx_polling || path.sending) { return 0; }
    path.tx_polling = true;
    ULONG completed{};
    const auto limit = budget < dma_ring_count ? budget : dma_ring_count;
    while (completed < limit && path.outstanding != 0) {
        const auto index = path.consumer;
        ULONG options{};
        const auto status = read_dma_completion(*path.dma, true, index, options);
        if (status == DmaStatus::device_owned) { break; }
        if (status != DmaStatus::ok || path.pending[index] == nullptr) { fault(path); break; }
        auto* packet = path.pending[index];
        path.pending[index] = nullptr;
        path.consumer = (index + 1) % dma_ring_count;
        --path.outstanding;
        ++completed;
        ++path.counters.tx_completed;
        NdisMSendComplete(path.adapter, packet, NDIS_STATUS_SUCCESS);
    }
    const bool notify = path.blocked && path.outstanding < dma_ring_count && path.running;
    if (notify) { path.blocked = false; NdisMSendResourcesAvailable(path.adapter); }
    path.tx_polling = false;
    return completed;
}
ULONG poll_receive(Datapath& path, ULONG budget) noexcept
{
    if (!valid_dispatch() || path.adapter == nullptr || !path.running || path.rx_polling) { return 0; }
    path.rx_polling = true;
    ULONG consumed{};
    const auto limit = budget < dma_ring_count ? budget : dma_ring_count;
    while (consumed < limit && path.running) {
        const auto index = path.receive_index;
        ULONG options{};
        const auto status = read_dma_completion(*path.dma, false, index, options);
        if (status == DmaStatus::device_owned) { break; }
        if (status != DmaStatus::ok) { fault(path); break; }
        const auto size = received_frame_size(options, dma_packet_size);
        if (size < minimum_tx_size || copy_received_packet(*path.dma, index, path.receive_copy,
                sizeof(path.receive_copy), static_cast<ULONG>(size)) != DmaStatus::ok ||
            (path.receive_copy[12] == 0x81 && path.receive_copy[13] == 0x00) ||
            (path.receive_copy[12] == 0x88 && path.receive_copy[13] == 0xa8)) {
            ++path.counters.rx_invalid;
        } else if (!accepts_frame(path.filter, path.station, path.receive_copy, size)) {
            ++path.counters.rx_filtered;
        } else {
            NdisAdjustBufferLength(path.receive_buffer, static_cast<UINT>(size));
            path.receive_packet->Private.ValidCounts = FALSE;
            NDIS_SET_PACKET_STATUS(path.receive_packet, NDIS_STATUS_RESOURCES);
            NDIS_SET_PACKET_HEADER_SIZE(path.receive_packet, ethernet_header_size);
            NDIS_PER_PACKET_INFO_FROM_PACKET(path.receive_packet, TcpIpChecksumPacketInfo) = nullptr;
            NDIS_PER_PACKET_INFO_FROM_PACKET(path.receive_packet, Ieee8021QInfo) = nullptr;
            // RESOURCES forbids retaining the packet or returning it later.
            // The ring slot remains unarmed throughout this synchronous call.
            auto* packet = path.receive_packet;
            NdisMIndicateReceivePacket(path.adapter, &packet, 1);
            ++path.counters.rx_indicated;
        }
        ++consumed;
        path.receive_index = (index + 1) % dma_ring_count;
        if (path.running && arm_receive(*path.dma, index) != DmaStatus::ok) { fault(path); break; }
    }
    path.rx_polling = false;
    return consumed;
}
NDIS_STATUS abort_transmits(Datapath& path, NDIS_STATUS status) noexcept
{
    if (!valid_dispatch() || path.adapter == nullptr || path.running || callback_active(path) ||
        path.dma->device_accessible() || status == NDIS_STATUS_SUCCESS || status == NDIS_STATUS_PENDING) {
        return NDIS_STATUS_FAILURE;
    }
    path.tx_polling = true;
    for (ULONG index = 0; index < dma_ring_count; ++index) {
        auto* packet = path.pending[index];
        if (packet != nullptr) {
            path.pending[index] = nullptr;
            --path.outstanding;
            ++path.counters.tx_aborted;
            NdisMSendComplete(path.adapter, packet, status);
        }
    }
    path.producer = path.consumer = 0;
    path.blocked = false;
    path.tx_polling = false;
    return NDIS_STATUS_SUCCESS;
}
NDIS_STATUS release_datapath(Datapath& path) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || path.running || callback_active(path) ||
        path.outstanding != 0 || (path.dma != nullptr && path.dma->device_accessible())) {
        return NDIS_STATUS_FAILURE;
    }
    free_receive(path);
    path.adapter = nullptr;
    path.dma = nullptr;
    path.resources = nullptr;
    path.faulted = false;
    path.blocked = false;
    path.producer = path.consumer = path.receive_index = 0;
    path.filter = {};
    return NDIS_STATUS_SUCCESS;
}
NDIS_STATUS query_datapath_information(const Datapath& path, NDIS_OID oid, PVOID output,
                                      ULONG length, PULONG written, PULONG needed) noexcept
{
    if (written == nullptr || needed == nullptr) { return NDIS_STATUS_INVALID_DATA; }
    *written = *needed = 0;
    if (!valid_dispatch() || path.adapter == nullptr) { return NDIS_STATUS_ADAPTER_NOT_READY; }
    const void* source{};
    std::size_t size{};
    ULONG scalar{};
    switch (oid) {
    case OID_GEN_CURRENT_PACKET_FILTER: scalar = path.filter.flags; source = &scalar; size = 4; break;
    case OID_802_3_MULTICAST_LIST:
        source = path.filter.multicast; size = path.filter.multicast_count * 6; break;
    case OID_802_3_MAXIMUM_LIST_SIZE: scalar = maximum_multicast_addresses; source = &scalar; size = 4; break;
    case OID_802_3_CURRENT_ADDRESS: case OID_802_3_PERMANENT_ADDRESS: source = path.station; size = 6; break;
    default: return NDIS_STATUS_NOT_SUPPORTED;
    }
    const auto result = copy_information(output, length, source, size);
    *written = static_cast<ULONG>(result.transferred);
    *needed = static_cast<ULONG>(result.needed);
    return information_status(result.status);
}
NDIS_STATUS set_datapath_information(Datapath& path, NDIS_OID oid, PVOID input,
                                    ULONG length, PULONG read, PULONG needed) noexcept
{
    if (read == nullptr || needed == nullptr) { return NDIS_STATUS_INVALID_DATA; }
    *read = *needed = 0;
    if (!valid_dispatch() || path.adapter == nullptr || path.faulted) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }
    auto next = path.filter;
    InformationResult result{};
    switch (oid) {
    case OID_GEN_CURRENT_PACKET_FILTER: result = set_packet_filter(input, length, next); break;
    case OID_802_3_MULTICAST_LIST: result = set_multicast_list(input, length, next); break;
    default: return NDIS_STATUS_NOT_SUPPORTED;
    }
    *needed = static_cast<ULONG>(result.needed);
    if (result.status != InformationStatus::success) { return information_status(result.status); }
    if (path.running && !program_filter(path, next)) { return NDIS_STATUS_FAILURE; }
    path.filter = next;
    *read = static_cast<ULONG>(result.transferred);
    return NDIS_STATUS_SUCCESS;
}
} // namespace rtl8125::reactos
