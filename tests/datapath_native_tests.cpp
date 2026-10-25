// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.datapath_native
Purpose: Check real-SDK packet chains, ring wrap and synchronous receive ABI.
Created: 2026-10-24
Architecture: User-mode injected NDIS/NT imports and memory-only NIC model.
No physical DMA, PCI, guest networking, peer traffic or reset proof is implied.
*/
#include "../src/reactos/adapter.hpp"
#include "../src/core/descriptor_bits.hpp"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <new>

extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);
void interrupt_fixture_drain_work() noexcept;
void interrupt_fixture_drain_dpc() noexcept;

namespace {
using namespace rtl8125;
using namespace rtl8125::reactos;
struct PacketFixture {
    NDIS_PACKET packet;
    NDIS_PACKET_OOB_DATA oob;
    NDIS_PACKET_EXTENSION extension;
    MDL buffer;
    UCHAR bytes[maximum_frame_size];
};
struct Fixture {
    KIRQL irql{};
    unsigned allocations{}, frees{}, pool_step{}, fail_step{}, packet_frees{}, buffer_frees{}, pool_frees{};
    unsigned sends{}, resources_available{}, indications{}, doorbells{}, writes{}, checksum_clear{}, size_limit{};
    unsigned completions[257]{};
    bool bad{}, fail_doorbell{}, fail_filter{}, fail_flush{}, master{true}, reenter{}, null_buffer_pool{};
    Datapath* active{};
    NDIS_STATUS completion_status{};
    NDIS_MINIPORT_CHARACTERISTICS table{};
    PDRIVER_UNLOAD unload{};
    PVOID extension_key{};
    alignas(void*) UCHAR extension[64]{};
    Adapter* production{};
    unsigned memory_allocations{}, memory_frees{}, resource_releases{}, deferred{}, fences{}, terminated{};
    bool irq_ready{}, fail_activation{}, fail_fence{};
} f;
unsigned checks{}, failures{};
NDIS_MINIPORT_BLOCK miniport{};
alignas(256) UCHAR common[4][dma_ring_count * dma_packet_size]{};
alignas(16) UCHAR mmio[65536]{};
PacketFixture packets[257]{}, receive{};
alignas(Adapter) UCHAR adapter_storage[sizeof(Adapter)]{};
MDL extra[257]{}, extra_overflow[256]{};
const UCHAR station[]{2, 1, 2, 3, 4, 5};
void expect(bool value, const char* message) noexcept
{
    ++checks;
    if (!value) { ++failures; printf("FAIL: %s\n", message); }
}
NDIS_HANDLE adapter() noexcept { return &miniport; }
void mdl(MDL& buffer, PVOID address, ULONG length) noexcept
{
    memset(&buffer, 0, sizeof(buffer));
    buffer.MdlFlags = MDL_MAPPED_TO_SYSTEM_VA;
    buffer.MappedSystemVa = address;
    buffer.ByteCount = length;
}
void packet(PacketFixture& value, unsigned index, ULONG size = 60) noexcept
{
    memset(&value, 0, sizeof(value));
    value.packet.Private.NdisPacketOobOffset = static_cast<USHORT>(offsetof(PacketFixture, oob));
    mdl(value.buffer, value.bytes, size);
    value.packet.Private.Head = value.packet.Private.Tail = &value.buffer;
    memcpy(value.bytes, station, 6);
    value.bytes[6] = 2;
    value.bytes[12] = 0x08;
    value.bytes[13] = 0x06;
    for (unsigned byte = 14; byte < sizeof(value.bytes); ++byte) { value.bytes[byte] = static_cast<UCHAR>(index + byte); }
}
void reset(unsigned fail_step = 0) noexcept
{
    f = {};
    f.fail_step = fail_step;
    f.master = true;
    memset(common, 0, sizeof(common));
    memset(mmio, 0, sizeof(mmio));
    ULONG config = 0xa0c0003f;
    memcpy(mmio + 0x44, &config, 4);
    mmio[0xd3] = 0x30;
    USHORT drained = 0x0103;
    memcpy(mmio + 0xe2, &drained, 2);
    USHORT cplus = 0x8f3f;
    memcpy(mmio + 0xe0, &cplus, 2);
    memset(extra, 0, sizeof(extra));
    for (unsigned index = 0; index < 257; ++index) { packet(packets[index], index); }
    packet(receive, 0);
}
DeviceResources resources() noexcept
{
    DeviceResources value{};
    value.mapped_address = mmio;
    value.mapped_length = sizeof(mmio);
    value.chip = {ChipStatus::supported, MacVersion::rtl8125b, 0x641};
    value.interrupt_vector = 11; value.interrupt_level = 11; value.interrupt_affinity = 1;
    value.interrupt_shared = TRUE;
    return value;
}
KIRQL NTAPI mock_irql() { return f.irql; }
NDIS_STATUS NTAPI mock_dma_init(NDIS_HANDLE handle, BOOLEAN wide, ULONG size)
{
    f.bad |= handle != adapter() || wide || size != 524288;
    return NDIS_STATUS_SUCCESS;
}
VOID NTAPI mock_common(NDIS_HANDLE handle, ULONG length, BOOLEAN cached, PVOID* address,
                        PNDIS_PHYSICAL_ADDRESS logical)
{
    f.bad |= handle != adapter() || cached || f.allocations >= 4 ||
        length != (f.allocations < 2 ? 4096u : 524288u);
    const auto index = f.allocations++;
    *address = common[index];
    logical->QuadPart = 0x100000 + index * 0x100000;
}
VOID NTAPI mock_common_free(NDIS_HANDLE handle, ULONG, BOOLEAN cached, PVOID address,
                             NDIS_PHYSICAL_ADDRESS)
{
    f.bad |= handle != adapter() || cached;
    bool found{};
    for (auto& buffer : common) { found |= address == buffer; }
    f.bad |= !found;
    ++f.frees;
}
bool fail_pool() noexcept { return ++f.pool_step == f.fail_step; }
VOID NTAPI mock_packet_pool(PNDIS_STATUS status, PNDIS_HANDLE handle, UINT count, UINT reserved)
{
    f.bad |= count != 1 || reserved != 0;
    *status = fail_pool() ? NDIS_STATUS_RESOURCES : NDIS_STATUS_SUCCESS;
    *handle = *status == NDIS_STATUS_SUCCESS ? reinterpret_cast<NDIS_HANDLE>(1) : nullptr;
}
VOID NTAPI mock_buffer_pool(PNDIS_STATUS status, PNDIS_HANDLE handle, UINT count)
{
    f.bad |= count != 1;
    *status = fail_pool() ? NDIS_STATUS_RESOURCES : NDIS_STATUS_SUCCESS;
    *handle = *status == NDIS_STATUS_SUCCESS && !f.null_buffer_pool ? reinterpret_cast<NDIS_HANDLE>(2) : nullptr;
}
VOID NTAPI mock_packet(PNDIS_STATUS status, PNDIS_PACKET* output, NDIS_HANDLE pool)
{
    f.bad |= pool != reinterpret_cast<NDIS_HANDLE>(1);
    *status = fail_pool() ? NDIS_STATUS_RESOURCES : NDIS_STATUS_SUCCESS;
    *output = *status == NDIS_STATUS_SUCCESS ? &receive.packet : nullptr;
}
VOID NTAPI mock_buffer(PNDIS_STATUS status, PNDIS_BUFFER* output, NDIS_HANDLE pool,
                       PVOID address, UINT length)
{
    f.bad |= pool != (f.null_buffer_pool ? nullptr : reinterpret_cast<NDIS_HANDLE>(2)) || length != maximum_frame_size;
    *status = fail_pool() ? NDIS_STATUS_RESOURCES : NDIS_STATUS_SUCCESS;
    mdl(receive.buffer, address, length);
    *output = *status == NDIS_STATUS_SUCCESS ? &receive.buffer : nullptr;
}
VOID NTAPI mock_packet_free(PNDIS_PACKET value)
{
    f.bad |= value != &receive.packet || value->Private.Head != nullptr;
    ++f.packet_frees;
}
VOID NTAPI mock_buffer_free(PMDL value)
{
    f.bad |= value != &receive.buffer;
    ++f.buffer_frees;
}
VOID NTAPI mock_pool_free(NDIS_HANDLE pool)
{
    f.bad |= pool != reinterpret_cast<NDIS_HANDLE>(1) && (pool != reinterpret_cast<NDIS_HANDLE>(2) && !(f.null_buffer_pool && pool == nullptr));
    ++f.pool_frees;
}
PVOID NTAPI mock_map(PMDL, KPROCESSOR_MODE, MEMORY_CACHING_TYPE, PVOID, ULONG, ULONG) { return nullptr; }
VOID NTAPI mock_sleep(ULONG value)
{ f.bad |= value != 100 && value != 1000 && value != 2000; if (value == 1000) { interrupt_fixture_drain_work(); } }
VOID NTAPI mock_stall(ULONG value) { f.bad |= value == 0 || value > 50; }
ULONGLONG NTAPI mock_time() { return 1000; }
ULONG NTAPI mock_pci_read(NDIS_HANDLE handle, ULONG slot, ULONG offset, PVOID output, ULONG length)
{
    f.bad |= handle != adapter() || slot != 0 || length == 0 || length > 6 || offset > 256 - length;
    UCHAR config[256]{};
    config[0] = 0xec; config[1] = 0x10; config[2] = 0x25; config[3] = 0x81;
    config[4] = static_cast<UCHAR>(f.master ? 6 : 2);
    config[6] = 0x10; config[0x34] = 0x40; config[0x40] = 0x10; config[0x42] = 2;
    memcpy(output, config + offset, length);
    return length;
}
ULONG NTAPI mock_pci_write(NDIS_HANDLE handle, ULONG slot, ULONG offset, PVOID input, ULONG length)
{
    f.bad |= handle != adapter() || slot != 0 || offset != 4 || length != 2;
    f.master = (static_cast<UCHAR*>(input)[0] & 4) != 0;
    return length;
}
bool cpu_fence(void*) noexcept { return true; }
VOID NTAPI complete(NDIS_HANDLE handle, PNDIS_PACKET value, NDIS_STATUS status)
{
    f.bad |= handle != adapter();
    unsigned index = 257;
    for (unsigned candidate = 0; candidate < 257; ++candidate) {
        if (&packets[candidate].packet == value) { index = candidate; break; }
    }
    if (index == 257) { f.bad = true; return; }
    ++f.completions[index];
    f.bad |= f.completions[index] != 1;
    ++f.sends;
    f.completion_status = status;
    if (f.reenter) {
        f.reenter = false;
        expect(poll_transmit(*f.active, 256) == 0, "completion rejects nested poll");
        expect(release_datapath(*f.active) == NDIS_STATUS_FAILURE, "completion cannot release live callback state");
        expect(send_packet(*f.active, &packets[256].packet) == NDIS_STATUS_PENDING,
               "completion may synchronously submit another packet without double ownership");
    }
}
VOID NTAPI available(NDIS_HANDLE handle) { f.bad |= handle != adapter(); ++f.resources_available; }
VOID NTAPI indicate(NDIS_HANDLE handle, PPNDIS_PACKET values, UINT count)
{
    f.bad |= handle != adapter() || count != 1 || values[0] != &receive.packet ||
        NDIS_GET_PACKET_STATUS(values[0]) != NDIS_STATUS_RESOURCES ||
        NDIS_GET_PACKET_HEADER_SIZE(values[0]) != 14 || values[0]->Private.Head != &receive.buffer ||
        receive.buffer.ByteCount != 60;
    const auto index = f.active->receive_index;
    auto& desc = static_cast<LiveDescriptor*>(f.active->dma->rx_ring().virtual_address)[index];
    f.bad |= (desc.options & descriptor_owned) != 0 ||
        memcmp(receive.buffer.MappedSystemVa, common[3] + index * dma_packet_size, 60) != 0;
    expect(poll_receive(*f.active, 1) == 0, "synchronous receive rejects nested drain");
    expect(release_datapath(*f.active) == NDIS_STATUS_FAILURE, "synchronous receive cannot free indicated packet");
    ++f.indications;
}
VOID NTAPI mock_wrapper(PNDIS_HANDLE output, PVOID, PVOID, PVOID) { *output = adapter(); }
VOID NTAPI mock_terminate(NDIS_HANDLE handle, PVOID) { f.bad |= handle != adapter(); ++f.terminated; }
NDIS_STATUS NTAPI mock_register(NDIS_HANDLE handle, PNDIS_MINIPORT_CHARACTERISTICS table, UINT size)
{
    f.bad |= handle != adapter() || size != sizeof(*table);
    f.table = *table;
    return NDIS_STATUS_SUCCESS;
}
VOID NTAPI mock_attributes(NDIS_HANDLE handle, NDIS_HANDLE context, UINT interval,
                           ULONG flags, NDIS_INTERFACE_TYPE type)
{
    f.bad |= handle != adapter() || context != adapter_storage || interval != 0 || flags != 0 || type != NdisInterfacePci;
}
NDIS_STATUS NTAPI mock_memory(PVOID* output, UINT size, ULONG)
{
    f.bad |= size != sizeof(Adapter);
    *output = adapter_storage;
    ++f.memory_allocations;
    return NDIS_STATUS_SUCCESS;
}
VOID NTAPI mock_memory_free(PVOID pointer, UINT size, UINT flags)
{
    f.bad |= pointer != adapter_storage || size != sizeof(Adapter) || flags != 0;
    ++f.memory_frees;
}
NTSTATUS NTAPI mock_extension(PDRIVER_OBJECT, PVOID key, ULONG size, PVOID* output)
{
    f.bad |= size > sizeof(f.extension);
    *output = f.extension;
    f.extension_key = key;
    return STATUS_SUCCESS;
}
PVOID NTAPI mock_extension_get(PDRIVER_OBJECT, PVOID key)
{ f.bad |= key != f.extension_key; return f.extension; }
VOID NTAPI mock_unload(NDIS_HANDLE handle, PDRIVER_UNLOAD callback)
{ f.bad |= handle != adapter(); f.unload = callback; }
bool platform_ready(void*) noexcept { return f.irq_ready; }
bool platform_activate(void* context, DmaOwner& dma) noexcept
{
    auto* value = static_cast<Adapter*>(context);
    f.bad |= &dma != &value->dma || dma.tx_ring().logical_address.QuadPart != 0x100000 ||
        dma.rx_ring().logical_address.QuadPart != 0x200000;
    value->hardware.device.running = !f.fail_activation;
    return !f.fail_activation;
}
bool platform_fence(void* context) noexcept
{ ++f.fences; return !f.fail_fence && retire_interrupt(*static_cast<Adapter*>(context)); }
void platform_defer(void*) noexcept { ++f.deferred; }
Adapter* owned_adapter() noexcept
{
    auto* value = new (adapter_storage) Adapter{};
    value->handle = adapter();
    value->resources = resources();
    value->hardware.device.version = MacVersion::rtl8125b;
    value->hardware.device.initialized = true;
    memcpy(value->hardware.device.permanent_address, station, 6);
    f.production = value;
    f.active = &value->datapath;
    return value;
}
void handlers() noexcept
{
    miniport.SendCompleteHandler = complete;
    miniport.SendResourcesHandler = available;
    miniport.PacketIndicateHandler = indicate;
}
void cleanup(Datapath& path, DmaOwner& dma, const DeviceResources& device) noexcept
{
    stop_datapath(path);
    expect(abort_transmits(path, NDIS_STATUS_FAILURE) == NDIS_STATUS_FAILURE,
           "active DMA prevents abort even after CPU admission stopped");
    expect(retire_dma(dma, device, cpu_fence, nullptr) == DmaStatus::ok,
           "modeled reset/PCI guards retire DMA for fixture cleanup only");
    expect(abort_transmits(path, NDIS_STATUS_FAILURE) == NDIS_STATUS_SUCCESS,
           "positively retired model permits bounded pending abort");
    expect(release_datapath(path) == NDIS_STATUS_SUCCESS && f.packet_frees == 1 &&
           f.buffer_frees == 1 && f.pool_frees == 2, "receive packet MDL and pools freed once");
    expect(release_datapath(path) == NDIS_STATUS_SUCCESS && f.packet_frees == 1,
           "release is idempotent");
    expect(release_dma(dma) == DmaStatus::ok && f.frees == 4 && !f.bad,
           "separate DMA owner releases after datapath teardown with genuine ABI");
}
}

namespace rtl8125::reactos {
NDIS_STATUS acquire_resources(NDIS_HANDLE handle, NDIS_HANDLE, DeviceResources& output) noexcept
{ f.bad |= handle != adapter(); output = resources(); return NDIS_STATUS_SUCCESS; }
void release_resources(NDIS_HANDLE handle, DeviceResources& output) noexcept
{ f.bad |= handle != adapter(); output = {}; ++f.resource_releases; }
bool read_mmio8(const DeviceResources&, ULONG offset, UCHAR& value) noexcept
{
    value = offset == 0xd3 ? 0x30 : 0; f.bad |= offset != 0x37 && offset != 0xd3; return true;
}
bool write_mmio8(const DeviceResources&, ULONG offset, UCHAR value) noexcept
{
    f.bad |= offset != 0x37 || (value != 0x10 && (value & 0x80) == 0); return true;
}
bool read_mmio16(const DeviceResources&, ULONG offset, USHORT& value) noexcept
{
    if (offset > sizeof(mmio) - 2 || offset % 2 != 0) { value = 0; return false; }
    memcpy(&value, mmio + offset, 2); return true;
}
bool read_mmio32(const DeviceResources&, ULONG offset, ULONG& value) noexcept
{
    if (offset > sizeof(mmio) - 4 || offset % 4 != 0) { value = 0; return false; }
    memcpy(&value, mmio + offset, 4); return true;
}
bool write_mmio16(const DeviceResources&, ULONG offset, USHORT value) noexcept
{
    ++f.writes;
    if (offset == 0x90) { f.bad |= value != 1; ++f.doorbells; if (f.fail_doorbell) { return false; } }
    if (offset == 0xe0) { f.bad |= (value & 0x20) != 0; ++f.checksum_clear; }
    if (offset == 0xda) { f.bad |= value != 1519; ++f.size_limit; }
    if (offset > sizeof(mmio) - 2 || offset % 2 != 0) { return false; }
    memcpy(mmio + offset, &value, 2); return true;
}
bool write_mmio32(const DeviceResources&, ULONG offset, ULONG value) noexcept
{
    ++f.writes;
    if (f.fail_filter || offset > sizeof(mmio) - 4 || offset % 4 != 0) { return false; }
    memcpy(mmio + offset, &value, 4); return true;
}
bool flush_posted_writes(const DeviceResources&) noexcept { return !f.fail_flush; }
}
#define RTL8125_MOCK(name, bytes, implementation) \
    decltype(&name) import_##name __asm__("__imp__" #name "@" #bytes) = implementation
extern "C" {
RTL8125_MOCK(NdisInitializeWrapper, 16, mock_wrapper);
RTL8125_MOCK(NdisTerminateWrapper, 8, mock_terminate);
RTL8125_MOCK(NdisMRegisterMiniport, 12, mock_register);
RTL8125_MOCK(NdisMSetAttributesEx, 20, mock_attributes);
RTL8125_MOCK(NdisAllocateMemoryWithTag, 12, mock_memory);
RTL8125_MOCK(NdisFreeMemory, 12, mock_memory_free);
RTL8125_MOCK(IoAllocateDriverObjectExtension, 16, mock_extension);
RTL8125_MOCK(IoGetDriverObjectExtension, 8, mock_extension_get);
RTL8125_MOCK(NdisMRegisterUnloadHandler, 8, mock_unload);
ULONG __cdecl fixture_debug(PCSTR, ...) __asm__("_DbgPrint");
ULONG __cdecl fixture_debug(PCSTR, ...) { return 0; }
RTL8125_MOCK(KeGetCurrentIrql, 0, mock_irql);
RTL8125_MOCK(NdisMInitializeScatterGatherDma, 12, mock_dma_init);
RTL8125_MOCK(NdisMAllocateSharedMemory, 20, mock_common);
RTL8125_MOCK(NdisMFreeSharedMemory, 24, mock_common_free);
RTL8125_MOCK(NdisAllocatePacketPool, 16, mock_packet_pool);
RTL8125_MOCK(NdisAllocateBufferPool, 12, mock_buffer_pool);
RTL8125_MOCK(NdisAllocatePacket, 12, mock_packet);
RTL8125_MOCK(NdisAllocateBuffer, 20, mock_buffer);
RTL8125_MOCK(NdisFreePacket, 4, mock_packet_free);
RTL8125_MOCK(NdisFreePacketPool, 4, mock_pool_free);
RTL8125_MOCK(NdisFreeBufferPool, 4, mock_pool_free);
RTL8125_MOCK(IoFreeMdl, 4, mock_buffer_free);
RTL8125_MOCK(MmMapLockedPagesSpecifyCache, 24, mock_map);
RTL8125_MOCK(NdisMSleep, 4, mock_sleep);
RTL8125_MOCK(KeStallExecutionProcessor, 4, mock_stall);
RTL8125_MOCK(KeQueryInterruptTime, 0, mock_time);
RTL8125_MOCK(NdisReadPciSlotInformation, 20, mock_pci_read);
RTL8125_MOCK(NdisWritePciSlotInformation, 20, mock_pci_write);
}

int main()
{
    handlers();
    for (unsigned fail = 1; fail <= 4; ++fail) {
        reset(fail);
        DmaOwner dma;
        Datapath path;
        const auto device = resources();
        expect(acquire_dma(adapter(), dma) == DmaStatus::ok, "DMA prerequisite allocated");
        expect(acquire_datapath(adapter(), dma, device, station, path) == NDIS_STATUS_RESOURCES &&
               path.adapter == nullptr, "each RX allocation failure unwinds acquisition");
        expect(f.packet_frees == (fail == 4 ? 1u : 0u) && f.buffer_frees == 0 &&
               f.pool_frees == (fail > 2 ? 2u : fail - 1), "partial RX pool/packet cleanup exactly once");
        expect(release_datapath(path) == NDIS_STATUS_SUCCESS && release_dma(dma) == DmaStatus::ok && !f.bad,
               "failed acquisition remains releasable");
    }
    reset();
    {
        DmaOwner dma;
        Datapath path;
        const auto device = resources();
        expect(acquire_dma(adapter(), dma) == DmaStatus::ok &&
               acquire_datapath(adapter(), dma, device, station, path) == NDIS_STATUS_SUCCESS,
               "fixed receive pools acquired with no MMIO or descriptor publication");
        expect(f.writes == 0 && !dma.device_accessible(), "acquisition is hardware passive");
        ULONG used = 99, needed = 99, flags = filter_directed | filter_broadcast;
        expect(set_datapath_information(path, OID_GEN_CURRENT_PACKET_FILTER, &flags, 4, &used, &needed) ==
               NDIS_STATUS_SUCCESS && used == 4 && needed == 0 && f.writes == 0,
               "inactive filter OID changes values without MMIO");
        f.active = &path;
        expect(enable_dma_bus_master(dma, device) == DmaStatus::ok, "explicit PCI bus mastering precedes RX OWN publication");
        expect(start_datapath(path) == NDIS_STATUS_SUCCESS && f.checksum_clear == 1 && f.size_limit == 1,
               "explicit startup disables offloads bounds RX and arms rings");
        ULONG config{};
        memcpy(&config, mmio + 0x44, 4);
        expect(config == 0xa000000a, "filter programming preserves unrelated RX bits and clears error/VLAN");
        expect(start_datapath(path) == NDIS_STATUS_FAILURE, "duplicate startup cannot rearm live rings");
        expect(send_packet(path, nullptr) == NDIS_STATUS_INVALID_PACKET, "null packet rejected without ownership");
        packets[0].extension.NdisPacketInfo[TcpIpChecksumPacketInfo] = reinterpret_cast<PVOID>(1);
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_NOT_SUPPORTED, "checksum offload request refused");
        packets[0].extension.NdisPacketInfo[TcpIpChecksumPacketInfo] = nullptr;
        packets[0].extension.NdisPacketInfo[TcpLargeSendPacketInfo] = reinterpret_cast<PVOID>(1);
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_NOT_SUPPORTED, "large send request refused");
        packets[0].extension.NdisPacketInfo[TcpLargeSendPacketInfo] = nullptr;
        packets[0].extension.NdisPacketInfo[Ieee8021QInfo] = reinterpret_cast<PVOID>(1);
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_NOT_SUPPORTED, "VLAN insertion request refused");
        packet(packets[0], 0);
        packets[0].buffer.Next = &packets[0].buffer;
        packets[0].buffer.ByteCount = 0;
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_INVALID_PACKET, "zero-byte cyclic chain walk bounded");
        packet(packets[0], 0);
        packets[0].buffer.MdlFlags = 0;
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_RESOURCES, "failed safe MDL mapping returns no ownership");
        packet(packets[0], 0);
        packets[0].buffer.ByteCount = 1515;
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_INVALID_LENGTH, "oversize MDL rejected before copy");
        packet(packets[0], 0, 30);
        mdl(extra[0], packets[0].bytes + 30, 30);
        packets[0].buffer.Next = &extra[0];
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_PENDING &&
               memcmp(common[2], packets[0].bytes, 60) == 0,
               "two-buffer chain gathers exact payload before OWN");
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_INVALID_PACKET,
               "duplicate pending packet never accepted twice");
        packet(packets[1], 1, 14);
        packet(packets[2], 2, 42);
        packets[2].bytes[12] = 8;
        packets[2].bytes[13] = 0;
        packets[2].bytes[14] = 0x45;
        packets[2].bytes[20] = packets[2].bytes[21] = 0;
        packets[2].bytes[23] = 17;
        packets[2].bytes[36] = 1;
        packets[2].bytes[37] = 0x3f;
        for (unsigned index = 1; index < dma_ring_count; ++index) {
            expect(send_packet(path, &packets[index].packet) == NDIS_STATUS_PENDING,
                   "full ring submits unique packets");
        }
        const auto* prepared = static_cast<LiveDescriptor*>(dma.tx_ring().virtual_address);
        expect((prepared[1].options & descriptor_length_mask) == 60 &&
               common[2][dma_packet_size + 14] == 0 && common[2][dma_packet_size + 59] == 0,
               "native short ARP gather pads to minimum before publication");
        expect((prepared[2].options & descriptor_length_mask) == 81 &&
               common[2][2 * dma_packet_size + 42] == 0 && common[2][2 * dma_packet_size + 80] == 0,
               "native IPv4 UDP/PTP gather applies exact Linux workaround");
        expect(path.outstanding == 256 && f.doorbells == 256 && f.sends == 0,
               "accepted packets remain pending and checked 16-bit doorbells issued");
        expect(send_packet(path, &packets[256].packet) == NDIS_STATUS_RESOURCES && path.blocked,
               "ring-full backpressure retains NDIS ownership");
        expect(poll_transmit(path, 256) == 0, "OWN blocks completion");
        auto* tx = static_cast<LiveDescriptor*>(dma.tx_ring().virtual_address);
        for (unsigned index = 0; index < dma_ring_count; ++index) { tx[index].options &= ~descriptor_owned; }
        f.reenter = true;
        expect(poll_transmit(path, 1) == 1 && path.outstanding == 256,
               "one budgeted completion plus reentrant submit wraps producer safely");
        expect(f.sends == 1 && f.resources_available == 0, "reentrant refill keeps ring backpressure");
        expect(poll_transmit(path, 255) == 255 && f.resources_available == 1 && path.outstanding == 1,
               "space notification follows actual progress once");
        expect(poll_transmit(path, 256) == 0, "new generation of wrapped slot waits for hardware");
        tx[0].options &= ~descriptor_owned;
        expect(poll_transmit(path, 999) == 1 && f.sends == 257 && path.outstanding == 0 &&
               poll_transmit(path, 256) == 0, "all accepted packets complete exactly once across wrap");
        auto* rx = static_cast<LiveDescriptor*>(dma.rx_ring().virtual_address);
        for (unsigned index = 0; index < dma_ring_count; ++index) {
            memcpy(common[3] + index * dma_packet_size, packets[0].bytes, 60);
            rx[index].options = descriptor_first | descriptor_last | 64;
        }
        expect(poll_receive(path, 0) == 0 && poll_receive(path, 3) == 3 && f.indications == 3,
               "RX zero and finite budgets respected");
        expect(poll_receive(path, 999) == 253 && f.indications == 256 && path.receive_index == 0,
               "synchronous copied RX wraps and consumes no newly rearmed entries");
        expect((rx[0].options & descriptor_owned) != 0 && poll_receive(path, 256) == 0,
               "RX rearm occurs after indication and OWN stops drain");
        const ULONG bad_options[]{descriptor_first | descriptor_last | receive_error | 64,
            descriptor_first | 64, descriptor_first | descriptor_last | 3,
            descriptor_first | descriptor_last | 1519};
        for (unsigned index = 0; index < 4; ++index) { rx[index].options = bad_options[index]; }
        expect(poll_receive(path, 4) == 4 && path.counters.rx_invalid == 4 && f.indications == 256,
               "error fragmented underflow and oversize descriptors rearmed without indication");
        memcpy(common[3] + 4 * dma_packet_size, packets[0].bytes, 60);
        common[3][0] = 0;
        auto* foreign = common[3] + 4 * dma_packet_size;
        foreign[5] ^= 1;
        rx[4].options = descriptor_first | descriptor_last | 64;
        expect(poll_receive(path, 1) == 1 && path.counters.rx_filtered == 1,
               "foreign unicast dropped by exact software filter");
        flags = filter_promiscuous;
        expect(set_datapath_information(path, OID_GEN_CURRENT_PACKET_FILTER, &flags, 4, &used, &needed) ==
               NDIS_STATUS_SUCCESS && used == 4, "active promiscuous filter program succeeds");
        memcpy(common[3] + 5 * dma_packet_size, foreign, 60);
        rx[5].options = descriptor_first | descriptor_last | 64;
        expect(poll_receive(path, 1) == 1 && f.indications == 257, "promiscuous admits foreign unicast");
        memcpy(common[3] + 6 * dma_packet_size, packets[0].bytes, 60);
        rx[6].options = descriptor_first | descriptor_last | 18;
        expect(poll_receive(path, 1) == 1 && path.counters.rx_invalid == 5 && f.indications == 257,
               "runt frame never enters ReactOS legacy zero-lookahead indication");
        memcpy(common[3] + 7 * dma_packet_size, packets[0].bytes, 60);
        common[3][7 * dma_packet_size + 12] = 0x81;
        common[3][7 * dma_packet_size + 13] = 0;
        rx[7].options = descriptor_first | descriptor_last | 64;
        expect(poll_receive(path, 1) == 1 && path.counters.rx_invalid == 6 && f.indications == 257,
               "unsupported VLAN dropped even when promiscuous is enabled");
        UCHAR list[]{1, 0, 0x5e, 0, 0, 1};
        expect(set_datapath_information(path, OID_802_3_MULTICAST_LIST, list, 6, &used, &needed) ==
               NDIS_STATUS_SUCCESS && path.filter.multicast_count == 1, "multicast OID sets bounded list");
        UCHAR queried[8]{};
        expect(query_datapath_information(path, OID_802_3_MULTICAST_LIST, queried, 5, &used, &needed) ==
               NDIS_STATUS_BUFFER_TOO_SHORT && used == 0 && needed == 6,
               "list query reports exact capacity before copying");
        expect(query_datapath_information(path, OID_802_3_MULTICAST_LIST, queried, 8, &used, &needed) ==
               NDIS_STATUS_SUCCESS && used == 6 && memcmp(queried, list, 6) == 0,
               "list query returns exact accepted bytes");
        ULONG max_list{};
        expect(query_datapath_information(path, OID_802_3_MAXIMUM_LIST_SIZE, &max_list, 4, &used, &needed) ==
               NDIS_STATUS_SUCCESS && max_list == 32, "maximum-list OID matches fixed allocation policy");
        expect(query_datapath_information(path, OID_TCP_TASK_OFFLOAD, queried, 8, &used, &needed) ==
               NDIS_STATUS_NOT_SUPPORTED, "no offload capability advertised");
        flags = 0x10;
        expect(set_datapath_information(path, OID_GEN_CURRENT_PACKET_FILTER, &flags, 4, &used, &needed) ==
               NDIS_STATUS_NOT_SUPPORTED && used == 0 && path.filter.flags == filter_promiscuous,
               "unsupported filter rejected unchanged");
        f.irql = DISPATCH_LEVEL + 1;
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_ADAPTER_NOT_READY &&
               poll_receive(path, 1) == 0 && poll_transmit(path, 1) == 0,
               "packet APIs refuse above DISPATCH_LEVEL");
        f.irql = PASSIVE_LEVEL;
        cleanup(path, dma, device);
    }
    reset();
    {
        DmaOwner dma; Datapath path; const auto device = resources();
        expect(acquire_dma(adapter(), dma) == DmaStatus::ok &&
               acquire_datapath(adapter(), dma, device, station, path) == NDIS_STATUS_SUCCESS &&
               enable_dma_bus_master(dma, device) == DmaStatus::ok &&
               start_datapath(path) == NDIS_STATUS_SUCCESS, "doorbell fault fixture initialized");
        f.active = &path;
        f.fail_doorbell = true;
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_PENDING && path.outstanding == 1 &&
               !path.running && path.faulted && f.sends == 0,
               "post-OWN doorbell failure keeps accepted packet pending and stops admission");
        expect(abort_transmits(path, NDIS_STATUS_FAILURE) == NDIS_STATUS_FAILURE &&
               release_datapath(path) == NDIS_STATUS_FAILURE, "uncertain hardware cannot release pending ownership");
        cleanup(path, dma, device);
        expect(f.sends == 1 && f.completions[0] == 1 && f.completion_status == NDIS_STATUS_FAILURE,
               "retired failed send completed exactly once with error");
    }
    reset();
    {
        f.null_buffer_pool = true;
        DmaOwner dma; Datapath path; const auto device = resources();
        expect(acquire_dma(adapter(), dma) == DmaStatus::ok &&
               acquire_datapath(adapter(), dma, device, station, path) == NDIS_STATUS_SUCCESS &&
               enable_dma_bus_master(dma, device) == DmaStatus::ok &&
               start_datapath(path) == NDIS_STATUS_SUCCESS, "actual ReactOS successful null buffer pool fixture initialized");
        ULONG flags = filter_broadcast, used{}, needed{};
        f.fail_filter = true;
        expect(set_datapath_information(path, OID_GEN_CURRENT_PACKET_FILTER, &flags, 4, &used, &needed) ==
               NDIS_STATUS_FAILURE && used == 0 && path.filter.flags == 0 && path.faulted && !path.running,
               "partial hardware filter failure stops admission and leaves software transaction unchanged");
        f.fail_filter = false;
        cleanup(path, dma, device);
    }
    reset();
    {
        DmaOwner dma; Datapath path; const auto device = resources();
        expect(acquire_dma(adapter(), dma) == DmaStatus::ok &&
               acquire_datapath(adapter(), dma, device, station, path) == NDIS_STATUS_SUCCESS &&
               enable_dma_bus_master(dma, device) == DmaStatus::ok &&
               start_datapath(path) == NDIS_STATUS_SUCCESS, "chain-boundary fixture initialized");
        packets[0].buffer.ByteCount = 0;
        packets[0].buffer.Next = &extra[0];
        for (unsigned index = 0; index < 255; ++index) {
            mdl(extra[index], packets[0].bytes, index == 254 ? 60 : 0);
            extra[index].Next = index == 254 ? nullptr : &extra[index + 1];
        }
        expect(send_packet(path, &packets[0].packet) == NDIS_STATUS_PENDING &&
               memcmp(common[2], packets[0].bytes, 60) == 0,
               "exactly 256 MDLs including empty segments accepted with correct bytes");
        packets[1].buffer.ByteCount = 0;
        packets[1].buffer.Next = &extra_overflow[0];
        for (unsigned index = 0; index < 256; ++index) {
            mdl(extra_overflow[index], packets[1].bytes, 0);
            extra_overflow[index].Next = index == 255 ? nullptr : &extra_overflow[index + 1];
        }
        expect(send_packet(path, &packets[1].packet) == NDIS_STATUS_INVALID_PACKET && path.outstanding == 1,
               "257th MDL rejected before accepting NDIS packet ownership");
        cleanup(path, dma, device);
        expect(f.sends == 1 && f.completions[0] == 1 && f.completions[1] == 0,
               "chain-limit rejection produces no spurious completion");
    }
    reset();
    {
        expect(DriverEntry(nullptr, nullptr) == STATUS_SUCCESS, "production DriverEntry registers actual NDIS callbacks");
        const auto& callbacks = f.table.Ndis50Chars.Ndis40Chars.Ndis30Chars;
        expect(callbacks.HandleInterruptHandler != nullptr && callbacks.SendHandler != nullptr &&
               callbacks.ISRHandler != nullptr, "native M07 ISR and bounded packet DPC dispatch registered");
        NDIS_MEDIUM medium[]{NdisMedium802_3};
        NDIS_STATUS error{};
        UINT selected{};
        expect(callbacks.InitializeHandler(&error, &selected, medium, 1, adapter(), nullptr) ==
               NDIS_STATUS_NOT_SUPPORTED && f.memory_frees == 1 && f.allocations == 0 && f.writes == 0,
               "production Initialize remains read-only failclosed despite linked full packet code");
        auto* value = owned_adapter();
        PacketPrerequisites platform{value, platform_ready, platform_activate, platform_fence, platform_defer};
        expect(prepare_packet_path(*value, platform) == NDIS_STATUS_ADAPTER_NOT_READY && f.allocations == 0,
               "missing actual IRQ prerequisite refuses all packet allocation");
        f.irq_ready = true;
        value->hardware.device.initialized = false;
        expect(prepare_packet_path(*value, platform) == NDIS_STATUS_ADAPTER_NOT_READY,
               "uninitialized actual HardwareState refuses packet setup");
        value->hardware.device.initialized = true;
        expect(prepare_packet_path(*value, platform) == NDIS_STATUS_SUCCESS && f.writes == 0 &&
               value->phase == AdapterPhase::prepared, "production adapter owns DMA and RX pools before activation");
        ULONG flags = filter_directed | filter_broadcast, used{}, needed{};
        expect(callbacks.SetInformationHandler(value, OID_GEN_CURRENT_PACKET_FILTER, &flags, 4, &used, &needed) ==
               NDIS_STATUS_SUCCESS && value->packet_filter == flags && f.writes == 0,
               "actual registered Set dispatch installs prepared packet filter values");
        UCHAR address[6]{};
        expect(callbacks.QueryInformationHandler(value, OID_802_3_CURRENT_ADDRESS, address, 6, &used, &needed) ==
               NDIS_STATUS_SUCCESS && memcmp(address, station, 6) == 0,
               "registered MAC query returns real HardwareState address through owned packet path");
        expect(register_interrupt(*value) == NDIS_STATUS_SUCCESS, "production packet fixture registers its modeled assigned IRQ");
        expect(enable_dma_bus_master(value->dma, value->resources) == DmaStatus::ok, "production fixture explicitly enables verified PCI bus mastering");
        expect(activate_packet_path(*value) == NDIS_STATUS_SUCCESS && value->phase == AdapterPhase::running,
               "platform activation opens production admission only after HardwareState running");
        expect(enable_interrupt(*value) == NDIS_STATUS_SUCCESS, "modeled actual IRQ owner enables after packet activation");
        interrupt_fixture_drain_dpc(); interrupt_fixture_drain_work(); interrupt_fixture_drain_dpc();
        expect(callbacks.SendHandler(value, &packets[0].packet, 0) == NDIS_STATUS_ADAPTER_NOT_READY,
               "activation alone cannot invent a connected PHY link");
        hardware::LinkState link{};
        link.up = true; link.speed_mbps = 2500; link.full_duplex = true; link.autoneg_complete = true;
        expect(report_packet_link(*value, link), "actual PHY result enters guarded production link state");
        auto invalid_link = link;
        invalid_link.speed_mbps = 1234;
        expect(!report_packet_link(*value, invalid_link) && value->link_speed_100bps == 25000000,
               "invalid PHY speed cannot overwrite measured production link state");
        invalid_link = link;
        invalid_link.autoneg_complete = false;
        expect(!report_packet_link(*value, invalid_link), "unfinished autoneg cannot claim connected media");
        ULONG scalar{};
        expect(callbacks.QueryInformationHandler(value, OID_GEN_LINK_SPEED, &scalar, 4, &used, &needed) ==
               NDIS_STATUS_SUCCESS && scalar == 25000000, "production link OID translates measured Mbps to100bps");
        for (unsigned index = 0; index < 65; ++index) {
            expect(callbacks.SendHandler(value, &packets[index].packet, 0) == NDIS_STATUS_PENDING,
                   "actual registered Send enters adapter-owned copied TX path");
        }
        auto* tx = static_cast<LiveDescriptor*>(value->dma.tx_ring().virtual_address);
        auto* rx = static_cast<LiveDescriptor*>(value->dma.rx_ring().virtual_address);
        for (unsigned index = 0; index < 65; ++index) {
            tx[index].options &= ~descriptor_owned;
            memcpy(common[3] + index * dma_packet_size, packets[0].bytes, 60);
            rx[index].options = descriptor_first | descriptor_last | 64;
        }
        callbacks.HandleInterruptHandler(value);
        expect(f.sends == 64 && f.indications == 64 && value->interrupts.references == 1 && value->datapath.outstanding == 1,
               "registered DPC bounds TX/RX work and requests actual deferred continuation");
        callbacks.HandleInterruptHandler(value);
        expect(f.sends == 65 && f.indications == 65 && value->interrupts.references == 1,
               "second production DPC drains tail exactly once");
        ULONGLONG count{};
        expect(callbacks.QueryInformationHandler(value, OID_GEN_XMIT_OK, &count, 8, &used, &needed) ==
               NDIS_STATUS_SUCCESS && count == 65 && used == 8 && needed == 8,
               "actual counter OID reads completed production TX state");
        expect(callbacks.QueryInformationHandler(value, OID_GEN_RCV_OK, &count, 8, &used, &needed) ==
               NDIS_STATUS_SUCCESS && count == 65, "actual counter OID reads indicated production RX state");
        UCHAR multicast[]{1, 0, 0x5e, 0, 0, 1};
        expect(callbacks.SetInformationHandler(value, OID_802_3_MULTICAST_LIST, multicast, 6, &used, &needed) ==
               NDIS_STATUS_SUCCESS && value->datapath.filter.multicast_count == 1,
               "actual registered multicast Set dispatch programs bounded filters");
        expect(callbacks.SendHandler(value, &packets[65].packet, 0) == NDIS_STATUS_PENDING,
               "pending production send exists at halt boundary");
        const auto writes_before_removal = f.writes;
        f.table.PnPEventNotifyHandler(value, NdisDevicePnPEventSurpriseRemoved, nullptr, 0);
        expect(value->phase == AdapterPhase::quiescing && !value->datapath.running &&
               f.writes == writes_before_removal && f.completions[65] == 0,
               "actual surprise-removal callback closes admission without touching removed hardware");
        expect(callbacks.SetInformationHandler(value, OID_GEN_CURRENT_PACKET_FILTER, &flags, 4, &used, &needed) ==
               NDIS_STATUS_ADAPTER_NOT_READY && used == 0,
               "actual Set callback refuses mutations after admission teardown");
        f.fail_fence = true;
        callbacks.HaltHandler(value);
        expect(value->phase == AdapterPhase::retained && f.memory_frees == 1 && f.frees == 0 &&
               f.resource_releases == 1 && f.completions[65] == 0,
               "production Halt retains adapter mapping DMA and pending packet after failed CPU fence");
        callbacks.HandleInterruptHandler(value);
        expect(f.completions[65] == 0 && callbacks.SendHandler(value, &packets[66].packet, 0) ==
               NDIS_STATUS_ADAPTER_NOT_READY, "late DPC/send cannot restart retained adapter");
        f.fail_fence = false;
        callbacks.HaltHandler(value);
        expect(f.memory_frees == 2 && f.frees == 4 && f.resource_releases == 2 &&
               f.completions[65] == 1 && f.completion_status == NDIS_STATUS_FAILURE && !f.bad,
               "retried real Halt completes pending send and frees all owners only after model retirement");
        f.unload(nullptr);
        expect(f.terminated == 1 && !f.bad, "production wrapper unload remains exactly once");
    }
    reset();
    {
        expect(DriverEntry(nullptr, nullptr) == STATUS_SUCCESS, "partial activation fixture uses actual production registration");
        auto* value = owned_adapter();
        PacketPrerequisites platform{value, platform_ready, platform_activate, platform_fence, platform_defer};
        f.irq_ready = true;
        f.fail_activation = true;
        expect(prepare_packet_path(*value, platform) == NDIS_STATUS_SUCCESS &&
               enable_dma_bus_master(value->dma, value->resources) == DmaStatus::ok &&
               activate_packet_path(*value) == NDIS_STATUS_FAILURE && value->phase == AdapterPhase::incomplete &&
               !value->dma.allocated() && value->datapath.adapter == nullptr && f.fences == 1 && f.frees == 4 &&
               value->hardware.device.recovery_required,
               "failed real activation boundary retires published rings before releasing adapter-owned pools");
        f.table.AdapterShutdownHandler(value);
        expect(value->phase == AdapterPhase::quiescing && !value->link_up,
               "actual shutdown callback closes CPU admission without claiming M08 hardware shutdown");
        f.table.Ndis50Chars.Ndis40Chars.Ndis30Chars.HaltHandler(value);
        expect(f.memory_frees == 1 && f.resource_releases == 1 && f.frees == 4 && !f.bad,
               "halt after failed activation performs no double buffer release");
        f.unload(nullptr);
    }
    printf("%u genuine-SDK datapath mock checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
