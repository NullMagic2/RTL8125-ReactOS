// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.dma_native
Purpose: Exercise production DMA and firmware owners against real ReactOS ABI.
Created: 2026-10-24
Architecture: User-mode NDIS/NT imports and device model are mocks. No kernel,
hardware, DMA engine, interrupt or physical retirement evidence is implied.
*/
#include "../src/reactos/dma.hpp"
#include "../src/reactos/hardware_init.hpp"
#include <stdio.h>
#include <string.h>
#include <initializer_list>

namespace {
using namespace rtl8125::reactos;
enum class Fault { none, adapter, allocate, high_address, range_overflow, bus_alignment,
                   cpu_alignment, cpu_fence, reset_stuck, removed, config_short,
                   pci_write, pci_stuck, flush, file_open, file_query, file_huge,
                   file_alloc, file_read, file_short, file_format, fifo_stuck, secondary_stuck,
                   cap_missing, cap_cycle, cap_unaligned, cap_overrun, cap_short,
                   cap_version, cap_type, pending_stuck, pending_late, wrong_card,
                   memory_off, engine_active, all_ones_pending, overlapping_bus };
struct Fixture {
    Fault fault{};
    KIRQL irql{};
    unsigned dma_init{}, allocations{}, frees{}, fail_allocation{}, reset_reads{},
             pci_reads{}, pci_writes{}, cpu_fences{}, pending_reads{}, mmio_writes{},
             bm_containments{}, file_closes{}, pool_allocations{}, pool_frees{};
    bool bad{}, freed[4]{}, pci_master{true};
    UCHAR chip_command{};
    ULONG misc{}, rx_config{0xdeadbe3f};
    ULONGLONG time{1000};
};
Fixture f{};
unsigned checks{}, failures{};
const auto adapter = reinterpret_cast<NDIS_HANDLE>(0x1234);
alignas(256) UCHAR buffers[4][dma_ring_count * dma_packet_size]{};
alignas(16) UCHAR firmware_bytes[1024]{};
alignas(16) UCHAR firmware_pool[1024]{};
void expect(bool condition, const char* message) noexcept
{
    ++checks;
    if (!condition) { ++failures; printf("FAIL: %s\n", message); }
}
void reset(Fault fault = Fault::none) noexcept
{
    f = {};
    f.fault = fault;
    f.pci_master = true;
    memset(buffers, 0, sizeof(buffers));
    memset(firmware_bytes, 0, sizeof(firmware_bytes));
    firmware_bytes[0] = 0x23;
    firmware_bytes[1] = 0x01;
    firmware_bytes[2] = 1;
    firmware_bytes[3] = 0x80;
}
KIRQL NTAPI mock_irql() { return f.irql; }
ULONGLONG NTAPI mock_time() { return f.time * 10; }
NDIS_STATUS NTAPI mock_init(NDIS_HANDLE handle, BOOLEAN address64, ULONG maximum)
{
    ++f.dma_init;
    f.bad |= handle != adapter || address64 || maximum != dma_ring_count * dma_packet_size;
    return f.fault == Fault::adapter ? NDIS_STATUS_RESOURCES : NDIS_STATUS_SUCCESS;
}
VOID NTAPI mock_allocate(NDIS_HANDLE handle, ULONG length, BOOLEAN cached,
                         PVOID* address, PNDIS_PHYSICAL_ADDRESS logical)
{
    f.bad |= handle != adapter || cached || f.dma_init != 1 || f.allocations >= 8 ||
        length != (f.allocations % 4 < 2 ? 4096u : 524288u);
    const auto index = f.allocations++ % 4;
    f.freed[index] = false;
    *address = nullptr;
    logical->QuadPart = 0;
    if (f.fault == Fault::allocate && index == f.fail_allocation) { return; }
    *address = buffers[index];
    logical->QuadPart = 0x100000 + index * 0x100000;
    if (index == f.fail_allocation) {
        if (f.fault == Fault::high_address) { logical->HighPart = 1; }
        if (f.fault == Fault::range_overflow) { logical->QuadPart = 0xffffff00; }
        if (f.fault == Fault::bus_alignment) { ++logical->LowPart; }
        if (f.fault == Fault::overlapping_bus) { logical->QuadPart = 0x100000; }
        if (f.fault == Fault::cpu_alignment) { *address = buffers[index] + 1; }
    }
}
VOID NTAPI mock_free(NDIS_HANDLE handle, ULONG length, BOOLEAN cached,
                     PVOID address, NDIS_PHYSICAL_ADDRESS logical)
{
    f.bad |= handle != adapter || cached;
    unsigned index = 4;
    for (unsigned candidate = 0; candidate < 4; ++candidate) {
        if (address == buffers[candidate] || address == buffers[candidate] + 1) { index = candidate; }
    }
    if (index == 4) { f.bad = true; return; }
    f.bad |= f.freed[index] || length != (index < 2 ? 4096u : 524288u);
    auto expected = static_cast<LONGLONG>(0x100000 + index * 0x100000);
    if (index == f.fail_allocation) {
        if (f.fault == Fault::high_address) { expected += UINT64_C(1) << 32; }
        if (f.fault == Fault::range_overflow) { expected = 0xffffff00; }
        if (f.fault == Fault::bus_alignment) { ++expected; }
        if (f.fault == Fault::overlapping_bus) { expected = 0x100000; }
    }
    f.bad |= logical.QuadPart != expected;
    f.freed[index] = true;
    ++f.frees;
}
VOID NTAPI mock_sleep(ULONG value) { f.bad |= value != 1000 && value != 2000; f.time += value; }
VOID NTAPI mock_stall(ULONG value) { f.bad |= value == 0 || value > 50; f.time += value; }
ULONG NTAPI mock_read_pci(NDIS_HANDLE handle, ULONG slot, ULONG offset, PVOID bytes, ULONG length)
{
    ++f.pci_reads;
    f.bad |= handle != adapter || slot != 0 || length == 0 || length > 6 || offset > 256 - length;
    UCHAR config[256]{};
    config[0] = f.fault == Fault::wrong_card ? 0x99 : 0xec;
    config[1] = 0x10; config[2] = 0x25; config[3] = 0x81;
    config[4] = static_cast<UCHAR>((f.fault == Fault::memory_off ? 0 : 2) | (f.pci_master ? 4 : 0));
    config[5] = 1;
    config[6] = f.fault == Fault::cap_missing ? 0 : 0x10;
    config[0x34] = f.fault == Fault::cap_unaligned ? 0x41 : f.fault == Fault::cap_overrun ? 0xf4 : 0x40;
    config[0x40] = f.fault == Fault::cap_cycle ? 1 : 0x10;
    config[0x41] = f.fault == Fault::cap_cycle ? 0x40 : 0;
    config[0x42] = f.fault == Fault::cap_version ? 0 : f.fault == Fault::cap_type ? 0x42 : 2;
    if (offset == 0x4a) {
        ++f.pending_reads;
        config[0x4a] = f.fault == Fault::pending_stuck ||
            (f.fault == Fault::pending_late && f.pending_reads < 3) ? 0x20 :
            f.fault == Fault::all_ones_pending ? 0xff : 0;
        config[0x4b] = f.fault == Fault::all_ones_pending ? 0xff : 0;
    }
    memcpy(bytes, config + offset, length);
    return f.fault == Fault::config_short || (f.fault == Fault::cap_short && offset == 0x40) ? length - 1 : length;
}
ULONG NTAPI mock_write_pci(NDIS_HANDLE handle, ULONG slot, ULONG offset, PVOID bytes, ULONG length)
{
    ++f.pci_writes;
    f.bad |= handle != adapter || slot != 0 || offset != 4 || length != 2 ||
        static_cast<UCHAR*>(bytes)[1] != 1;
    const auto enabling = (static_cast<UCHAR*>(bytes)[0] & 4) != 0;
    if (!enabling) { ++f.bm_containments; }
    if (f.fault != Fault::pci_stuck) { f.pci_master = enabling; }
    return f.fault == Fault::pci_write ? 0 : length;
}
bool cpu_fence(void* context) noexcept
{
    ++f.cpu_fences;
    if (context != nullptr) {
        UCHAR frame[60]{};
        auto& owner = *static_cast<DmaOwner*>(context);
        f.bad |= publish_transmit(owner, 0, frame, sizeof(frame)) != DmaStatus::invalid_state ||
            arm_receive(owner, 0) != DmaStatus::invalid_state;
    }
    return f.fault != Fault::cpu_fence;
}
NTSTATUS NTAPI mock_create(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
                           PIO_STATUS_BLOCK status, PLARGE_INTEGER size, ULONG file_attributes,
                           ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG ea_length)
{
    f.bad |= access != (FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE) ||
        attributes->Attributes != (OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE) ||
        share != FILE_SHARE_READ || disposition != FILE_OPEN ||
        options != (FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE) ||
        size != nullptr || file_attributes != FILE_ATTRIBUTE_NORMAL || ea != nullptr || ea_length != 0;
    const WCHAR expected[] = L"\\SystemRoot\\system32\\drivers\\rtl_nic\\rtl8125b-2.fw";
    f.bad |= attributes->ObjectName->Length != sizeof(expected) - sizeof(WCHAR) ||
        memcmp(attributes->ObjectName->Buffer, expected, sizeof(expected)) != 0;
    *handle = reinterpret_cast<HANDLE>(0x6789);
    status->Status = STATUS_SUCCESS;
    return f.fault == Fault::file_open ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_query(HANDLE handle, PIO_STATUS_BLOCK, PVOID output, ULONG size,
                          FILE_INFORMATION_CLASS info)
{
    f.bad |= handle != reinterpret_cast<HANDLE>(0x6789) || size != sizeof(FILE_STANDARD_INFORMATION) ||
        info != FileStandardInformation;
    auto& value = *static_cast<FILE_STANDARD_INFORMATION*>(output);
    value = {};
    value.EndOfFile.QuadPart = f.fault == Fault::file_huge ? 1048577 : 4;
    return f.fault == Fault::file_query ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_read_file(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apc, PVOID context,
                              PIO_STATUS_BLOCK status, PVOID output, ULONG size,
                              PLARGE_INTEGER offset, PULONG key)
{
    f.bad |= handle != reinterpret_cast<HANDLE>(0x6789) || event != nullptr || apc != nullptr ||
        context != nullptr || size != 4 || offset->QuadPart != 0 || key != nullptr;
    memcpy(output, firmware_bytes, size);
    if (f.fault == Fault::file_format) { static_cast<UCHAR*>(output)[3] = 0xf0; }
    status->Information = f.fault == Fault::file_short ? 3 : size;
    return f.fault == Fault::file_read ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}
NTSTATUS NTAPI mock_close(HANDLE handle)
{
    ++f.file_closes;
    f.bad |= handle != reinterpret_cast<HANDLE>(0x6789);
    return STATUS_SUCCESS;
}
PVOID NTAPI mock_pool(POOL_TYPE type, SIZE_T size, ULONG)
{
    ++f.pool_allocations;
    f.bad |= type != NonPagedPool || size != 4;
    return f.fault == Fault::file_alloc ? nullptr : firmware_pool;
}
VOID NTAPI mock_pool_free(PVOID address, ULONG)
{
    ++f.pool_frees;
    f.bad |= address != firmware_pool;
}
}

namespace rtl8125::reactos {
bool read_mmio8(const DeviceResources& resources, ULONG offset, UCHAR& value) noexcept
{
    f.bad |= resources.mapped_address == nullptr;
    if (offset == 0x37) {
        ++f.reset_reads;
        value = f.fault == Fault::removed ? 0xff : f.fault == Fault::reset_stuck ? 0x10 :
            f.fault == Fault::engine_active ? 0x0c : f.chip_command;
        return true;
    }
    if (offset == 0xd3) { value = f.fault == Fault::fifo_stuck ? 0 : 0x30; return true; }
    f.bad = true; value = 0; return false;
}
bool write_mmio8(const DeviceResources& resources, ULONG offset, UCHAR value) noexcept
{
    ++f.mmio_writes;
    f.bad |= resources.mapped_address == nullptr || offset != 0x37 || (value != 0x10 && (value & 0x80) == 0);
    f.chip_command = value == 0x10 ? 0 : value;
    return true;
}
bool read_mmio16(const DeviceResources&, ULONG offset, USHORT& value) noexcept
{
    f.bad |= offset != 0xe2;
    value = f.fault == Fault::secondary_stuck ? 0 : 0x0103; return offset == 0xe2;
}
bool read_mmio32(const DeviceResources&, ULONG offset, ULONG& value) noexcept
{
    f.bad |= offset != 0x44 && offset != 0xf0;
    value = offset == 0x44 ? f.rx_config : f.misc; return offset == 0x44 || offset == 0xf0;
}
bool write_mmio16(const DeviceResources&, ULONG, USHORT) noexcept { return false; }
bool write_mmio32(const DeviceResources&, ULONG offset, ULONG value) noexcept
{
    ++f.mmio_writes;
    f.bad |= offset != 0x38 && offset != 0x44 && offset != 0xf0;
    if (offset == 0x38) { f.bad |= value != 0; }
    if (offset == 0x44) { f.rx_config = value; }
    if (offset == 0xf0) { f.misc = value; }
    return offset == 0x38 || offset == 0x44 || offset == 0xf0;
}
bool flush_posted_writes(const DeviceResources&) noexcept { return f.fault != Fault::flush; }
}

#define RTL8125_MOCK(name, bytes, implementation) \
    decltype(&name) import_##name __asm__("__imp__" #name "@" #bytes) = implementation
extern "C" {
RTL8125_MOCK(KeGetCurrentIrql, 0, mock_irql);
RTL8125_MOCK(KeQueryInterruptTime, 0, mock_time);
RTL8125_MOCK(NdisMInitializeScatterGatherDma, 12, mock_init);
RTL8125_MOCK(NdisMAllocateSharedMemory, 20, mock_allocate);
RTL8125_MOCK(NdisMFreeSharedMemory, 24, mock_free);
RTL8125_MOCK(NdisMSleep, 4, mock_sleep);
RTL8125_MOCK(KeStallExecutionProcessor, 4, mock_stall);
RTL8125_MOCK(NdisReadPciSlotInformation, 20, mock_read_pci);
RTL8125_MOCK(NdisWritePciSlotInformation, 20, mock_write_pci);
RTL8125_MOCK(ZwCreateFile, 44, mock_create);
RTL8125_MOCK(ZwQueryInformationFile, 20, mock_query);
RTL8125_MOCK(ZwReadFile, 36, mock_read_file);
RTL8125_MOCK(ZwClose, 4, mock_close);
RTL8125_MOCK(ExAllocatePoolWithTag, 12, mock_pool);
RTL8125_MOCK(ExFreePoolWithTag, 8, mock_pool_free);
}

int main()
{
    using namespace rtl8125::reactos;
    for (const auto fault : {Fault::allocate, Fault::high_address, Fault::range_overflow,
                            Fault::bus_alignment, Fault::cpu_alignment}) {
        for (unsigned index = 0; index < 4; ++index) {
            reset(fault);
            f.fail_allocation = index;
            DmaOwner owner;
            expect(acquire_dma(adapter, owner) != DmaStatus::ok && !owner.allocated(),
                   "allocation failure or invalid logical range rolls back owner");
            expect(f.allocations == index + 1 && f.frees == index + (fault == Fault::allocate ? 0u : 1u),
                   "each acquired buffer freed exactly once with original address");
            const auto frees = f.frees;
            expect(release_dma(owner) == DmaStatus::ok && f.frees == frees && !f.bad,
                   "failed acquisition release is idempotent and ABI accurate");
        }
    }
    for (unsigned index = 1; index < 4; ++index) {
        reset(Fault::overlapping_bus); f.fail_allocation = index; DmaOwner overlap;
        expect(acquire_dma(adapter, overlap) == DmaStatus::invalid_address && !overlap.allocated() &&
               f.frees == index + 1 && !f.bad, "overlapping logical common buffers reject and unwind exact original addresses");
    }
    for (const auto fault : {Fault::wrong_card, Fault::memory_off, Fault::engine_active,
                            Fault::cap_missing, Fault::cap_cycle, Fault::cap_unaligned, Fault::cap_overrun,
                            Fault::cap_short, Fault::cap_version, Fault::cap_type, Fault::pending_stuck}) {
        reset(); DmaOwner disabled;
        expect(acquire_dma(adapter, disabled) == DmaStatus::ok, "PCI contract fixture allocates before enable");
        DeviceResources resources{}; resources.mapped_address = buffers[0];
        resources.chip = {rtl8125::ChipStatus::supported, rtl8125::MacVersion::rtl8125b, 0x641};
        f.fault = fault;
        expect(enable_dma_bus_master(disabled, resources) == DmaStatus::invalid_state && f.pci_writes == 0 &&
               !disabled.device_accessible(), "unknown PCI identity, decoding, engines or malformed PCIe rejects before enable");
        expect(release_dma(disabled) == DmaStatus::ok && f.frees == 4 && !f.bad,
               "never enabled and never exposed invalid start releases only its own allocations");
    }
    for (const auto fault : {Fault::pci_write, Fault::pci_stuck, Fault::flush}) {
        reset(); DmaOwner uncertain;
        expect(acquire_dma(adapter, uncertain) == DmaStatus::ok, "partial PCI enable fixture acquires");
        DeviceResources resources{}; resources.mapped_address = buffers[0];
        resources.chip = {rtl8125::ChipStatus::supported, rtl8125::MacVersion::rtl8125a, 0x609};
        f.pci_master = false; f.fault = fault;
        expect(enable_dma_bus_master(uncertain, resources) == DmaStatus::retirement_failed &&
               uncertain.device_accessible() && release_dma(uncertain) == DmaStatus::retirement_failed && f.frees == 0,
               "partially failed PCI enable retains every owner rather than trusting reported write failure");
        f.fault = Fault::none; f.chip_command = 0;
        expect(retire_dma(uncertain, resources, cpu_fence, &uncertain) == DmaStatus::ok &&
               release_dma(uncertain) == DmaStatus::ok && f.frees == 4 && !f.bad,
               "partial PCI enable retries actual A-family drain before release");
    }
    reset();
    {
        DmaOwner retry;
        DeviceResources resources{}; resources.mapped_address = buffers[0];
        resources.chip = {rtl8125::ChipStatus::supported, rtl8125::MacVersion::rtl8125b, 0x641};
        expect(acquire_dma(adapter, retry) == DmaStatus::ok && enable_dma_bus_master(retry, resources) == DmaStatus::ok,
               "unexposed failed startup enables actual PCI contract");
        f.fault = Fault::pci_stuck;
        expect(release_dma(retry) == DmaStatus::retirement_failed && retry.device_accessible() && f.frees == 0,
               "failed unexposed bus-master cleanup retains allocations for controlled retirement");
        f.fault = Fault::none;
        expect(retire_dma(retry, resources, cpu_fence, &retry) == DmaStatus::ok && release_dma(retry) == DmaStatus::ok &&
               acquire_dma(adapter, retry) == DmaStatus::ok && f.dma_init == 1 && f.allocations == 8,
               "release/reacquire preserves the NDIS-owned DMA adapter across recovery retries");
        expect(acquire_dma(reinterpret_cast<NDIS_HANDLE>(0x9999), retry) == DmaStatus::invalid_state,
               "initialized owner cannot migrate to another NDIS wrapper");
        expect(release_dma(retry) == DmaStatus::ok && f.frees == 8 && !f.bad,
               "reacquired original buffers release once per generation");
    }
    reset();
    {
        DmaOwner invalid_fence;
        DeviceResources resources{}; resources.mapped_address = buffers[0];
        resources.chip = {rtl8125::ChipStatus::supported, rtl8125::MacVersion::rtl8125b, 0x641};
        expect(acquire_dma(adapter, invalid_fence) == DmaStatus::ok &&
               enable_dma_bus_master(invalid_fence, resources) == DmaStatus::ok &&
               expose_dma_rings(invalid_fence) == DmaStatus::ok, "ring-base exposure is tracked even before OWN");
        const auto writes = f.pci_writes + f.mmio_writes;
        expect(retire_dma(invalid_fence, resources, nullptr, nullptr) == DmaStatus::retirement_failed &&
               f.pci_writes + f.mmio_writes == writes && f.frees == 0,
               "absent actual CPU fence cannot issue concurrent hardware writes or free ring-base exposure");
        f.irql = DISPATCH_LEVEL;
        expect(retire_dma(invalid_fence, resources, cpu_fence, &invalid_fence) == DmaStatus::wrong_irql &&
               enable_dma_bus_master(invalid_fence, resources) == DmaStatus::wrong_irql && f.cpu_fences == 0,
               "retirement and PCI enable reject wrong IRQL before invoking fences or device I/O");
        f.irql = PASSIVE_LEVEL;
        expect(retire_dma(invalid_fence, resources, cpu_fence, &invalid_fence) == DmaStatus::ok &&
               release_dma(invalid_fence) == DmaStatus::ok && !f.bad,
               "real fence retry safely retires exposed ring bases after rejected callback");
    }
    reset(Fault::adapter);
    DmaOwner unavailable;
    expect(acquire_dma(adapter, unavailable) == DmaStatus::allocation_failure && f.allocations == 0,
           "failed DMA adapter prevents allocation");
    reset();
    DmaOwner owner;
    expect(acquire_dma(adapter, owner) == DmaStatus::ok && f.allocations == 4 && f.dma_init == 1,
           "adapter initialized once before four uncached common buffers");
    expect(acquire_dma(adapter, owner) == DmaStatus::invalid_state && f.allocations == 4,
           "reacquisition cannot overwrite owned DMA memory");
    DeviceResources resources{};
    resources.mapped_address = buffers[0];
    resources.chip = {rtl8125::ChipStatus::supported, rtl8125::MacVersion::rtl8125b, 0x641};
    UCHAR first_frame[60]{};
    expect(publish_transmit(owner, 0, first_frame, sizeof(first_frame)) == DmaStatus::invalid_state &&
           arm_receive(owner, 0) == DmaStatus::invalid_state && expose_dma_rings(owner) == DmaStatus::invalid_state,
           "no descriptor ownership before verified PCI bus-master enable");
    expect(enable_dma_bus_master(owner, resources) == DmaStatus::ok && !owner.device_accessible(),
           "explicit exact PCI Command-only enable verifies memory decoding and stopped engines");
    ULONG options = 99;
    UCHAR frame[1514]{};
    UCHAR received[1514]{};
    for (unsigned index = 0; index < sizeof(frame); ++index) { frame[index] = static_cast<UCHAR>(index); }
    for (ULONG index = 0; index < dma_ring_count; ++index) {
        expect(publish_transmit(owner, index, frame, sizeof(frame)) == DmaStatus::ok &&
               memcmp(buffers[2] + index * dma_packet_size, frame, sizeof(frame)) == 0,
               "all TX slots copy frame before publishing OWN");
        auto& tx = static_cast<LiveDescriptor*>(owner.tx_ring().virtual_address)[index];
        expect(tx.options == (0xb0000000u | (index == 255 ? 0x40000000u : 0) | 1514) &&
               tx.address_high == 0 && tx.address_low == 0x300000 + index * 2048,
               "live TX status, ring end and logical addresses are exact");
        expect(read_dma_completion(owner, true, index, options) == DmaStatus::device_owned && options == 0,
               "hardware-owned TX cannot be consumed or reused");
        expect(publish_transmit(owner, index, frame, 60) == DmaStatus::device_owned,
               "pending TX is never overwritten");
        tx.options &= ~0x80000000u;
        expect(read_dma_completion(owner, true, index, options) == DmaStatus::ok && options != 0 &&
               read_dma_completion(owner, true, index, options) == DmaStatus::not_submitted,
               "TX completion consumed once across full ring wrap");
        expect(arm_receive(owner, index) == DmaStatus::ok &&
               arm_receive(owner, index) == DmaStatus::device_owned, "RX arm owns slot until completion");
        auto& rx = static_cast<LiveDescriptor*>(owner.rx_ring().virtual_address)[index];
        memcpy(buffers[3] + index * dma_packet_size, frame, sizeof(frame));
        rx.options = 0x30000000 | 1518;
        expect(copy_received_packet(owner, index, received, sizeof(received), 1514) == DmaStatus::device_owned,
               "RX payload unavailable until acquire completion");
        expect(read_dma_completion(owner, false, index, options) == DmaStatus::ok &&
               copy_received_packet(owner, index, received, sizeof(received), 1514) == DmaStatus::ok &&
               memcmp(frame, received, sizeof(frame)) == 0, "RX completion exposes bounded payload after barrier");
    }
    expect(release_dma(owner) == DmaStatus::retirement_failed && f.frees == 0,
           "completed descriptors do not by themselves prove DMA engine retirement");
    expect(publish_transmit(owner, 256, frame, 60) == DmaStatus::invalid_argument &&
           publish_transmit(owner, 0, nullptr, 60) == DmaStatus::invalid_argument &&
           publish_transmit(owner, 0, frame, 59) == DmaStatus::invalid_argument &&
           publish_transmit(owner, 0, frame, 1515) == DmaStatus::invalid_argument,
           "TX bounds reject before touching descriptors");
    expect(copy_received_packet(owner, 0, received, 1513, 1514) == DmaStatus::invalid_argument &&
           copy_received_packet(owner, 0, received, 1514, 14) == DmaStatus::invalid_argument,
           "RX caller capacity and descriptor length are checked");
    for (const auto fault : {Fault::cpu_fence, Fault::reset_stuck, Fault::removed,
                             Fault::config_short, Fault::pci_write, Fault::pci_stuck, Fault::flush,
                             Fault::fifo_stuck, Fault::secondary_stuck, Fault::cap_missing,
                             Fault::cap_cycle, Fault::cap_unaligned, Fault::cap_overrun, Fault::cap_short,
                             Fault::cap_version, Fault::cap_type, Fault::pending_stuck, Fault::wrong_card,
                             Fault::memory_off, Fault::engine_active, Fault::all_ones_pending}) {
        f.fault = fault;
        f.pci_master = true;
        f.chip_command = 0;
        const auto before_containments = f.bm_containments;
        expect(retire_dma(owner, resources, cpu_fence, &owner) == DmaStatus::retirement_failed &&
               owner.device_accessible() && release_dma(owner) == DmaStatus::retirement_failed && f.frees == 0,
               "failed CPU fence/reset/removal/PCI proof retains every live common buffer");
        if (fault == Fault::fifo_stuck || fault == Fault::secondary_stuck || fault == Fault::reset_stuck ||
            fault == Fault::memory_off) {
            expect(f.bm_containments == before_containments + 1 && !f.pci_master,
                   "failed device drain still disables PCI bus mastering without granting retirement");
        }
        ULONG late_options = 99;
        expect(read_dma_completion(owner, true, 0, late_options) == DmaStatus::invalid_state && late_options == 0 &&
               copy_received_packet(owner, 0, received, sizeof(received), 1514) == DmaStatus::invalid_state,
               "latched retirement blocks late completion and payload reuse even after failure");
    }
    f.fault = Fault::pending_late;
    f.pending_reads = 0;
    f.chip_command = 0;
    expect(retire_dma(owner, resources, cpu_fence, &owner) == DmaStatus::ok && !owner.device_accessible() &&
           !f.pci_master, "full variant drain, reset, bus-master readback and bounded pending-request drain retire DMA");
    expect(f.pending_reads == 3, "delayed PCIe requests are drained with bounded passive waits");
    expect(publish_transmit(owner, 0, frame, 60) == DmaStatus::invalid_state &&
           arm_receive(owner, 0) == DmaStatus::invalid_state, "retired owner cannot publish late work");
    expect(release_dma(owner) == DmaStatus::ok && f.frees == 4 && release_dma(owner) == DmaStatus::ok &&
           f.frees == 4 && !f.bad, "retired owner frees exactly original buffers once");
    reset();
    DmaOwner passive;
    f.irql = DISPATCH_LEVEL;
    expect(acquire_dma(adapter, passive) == DmaStatus::wrong_irql &&
           release_dma(passive) == DmaStatus::wrong_irql && f.dma_init == 0,
           "shared-memory acquisition/free never calls NDIS outside PASSIVE_LEVEL");
    f.irql = PASSIVE_LEVEL;
    for (const auto fault : {Fault::none, Fault::file_open, Fault::file_query, Fault::file_huge,
                            Fault::file_alloc, Fault::file_read, Fault::file_short, Fault::file_format}) {
        reset(fault);
        FirmwareBlob blob{};
        const auto status = acquire_firmware(rtl8125::MacVersion::rtl8125b, blob);
        expect((status == NDIS_STATUS_SUCCESS) == (fault == Fault::none), "fixed-name NT firmware failure propagates");
        expect(f.file_closes == (fault == Fault::file_open ? 0u : 1u), "NT file handle closes on every opened path");
        release_firmware(blob);
        release_firmware(blob);
        expect(blob.bytes == nullptr && f.pool_frees ==
               (fault == Fault::none || fault == Fault::file_read || fault == Fault::file_short ||
                fault == Fault::file_format ? 1u : 0u) && !f.bad,
               "firmware storage lifetime and exact path/ABI verified");
    }
    printf("RTL8125 real-SDK DMA/firmware contracts: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
