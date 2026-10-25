// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.resources_native
Purpose: Inject NDIS failures into the production resource owner using real SDK ABI.
Created: 2026-10-24
Architecture: User-mode test executable. Imported OS entry points are local mocks;
no kernel, real PCI, hardware mapping, DMA, interrupt or guest evidence is implied.
*/
#include "../src/reactos/resources.hpp"
#include "../src/core/resources.hpp"

#include <stdio.h>
#include <string.h>

namespace {

enum class Fault {
    none, short_config, wrong_device, decode_disabled, query_size_failure,
    query_size_short, query_size_huge, allocation_failure, allocation_null,
    query_fill_failure, query_grew, invalid_count, short_aperture,
    wrong_resource, absent_interrupt, map_failure, map_failure_with_pointer,
    map_success_null, removed_chip, unknown_chip
};

struct Fixture {
    Fault fault{};
    unsigned allocations{};
    unsigned frees{};
    unsigned maps{};
    unsigned unmaps{};
    unsigned reads{};
    unsigned writes{};
    unsigned queries{};
    bool bad_contract{};
    NDIS_PHYSICAL_ADDRESS mapped_physical{};
};

Fixture fixture{};
unsigned checks{};
unsigned failures{};
alignas(4096) UCHAR registers[65536]{};
alignas(16) UCHAR resource_storage[256]{};
const auto adapter_handle = reinterpret_cast<NDIS_HANDLE>(0x1000);
const auto configuration_context = reinterpret_cast<NDIS_HANDLE>(0x2000);

void expect(bool condition, const char* message) noexcept
{
    ++checks;

    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", message);
    }
}

void put32(UCHAR* bytes, ULONG value) noexcept
{
    for (unsigned index = 0; index < 4; ++index) {
        bytes[index] = static_cast<UCHAR>(value >> (index * 8));
    }
}

ULONG NTAPI mock_read_pci(NDIS_HANDLE handle, ULONG slot, ULONG offset,
                          PVOID buffer, ULONG length)
{
    fixture.bad_contract |= handle != adapter_handle || slot != 0 || offset != 0 || length != 64;
    auto* config = static_cast<UCHAR*>(buffer);
    memset(buffer, 0, length);
    put32(config, fixture.fault == Fault::wrong_device ? 0x816810ec : 0x812510ec);
    config[4] = fixture.fault == Fault::decode_disabled ? 0 : 2;
    config[8] = 5;
    config[11] = 2;
    put32(config + 16, 0x1001);
    put32(config + 20, 0x23450004);
    put32(config + 24, 1);
    return fixture.fault == Fault::short_config ? length - 1 : length;
}

VOID NTAPI mock_query_resources(PNDIS_STATUS status, NDIS_HANDLE context,
                                 PNDIS_RESOURCE_LIST list, PUINT size)
{
    ++fixture.queries;
    fixture.bad_contract |= context != configuration_context;
    constexpr UINT required_size = FIELD_OFFSET(NDIS_RESOURCE_LIST, PartialDescriptors) +
                                   2 * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);

    if (list == nullptr) {
        fixture.bad_contract |= *size != 0;
        *size = fixture.fault == Fault::query_size_short ? 7 :
                fixture.fault == Fault::query_size_huge ? 65537 : required_size;
        *status = fixture.fault == Fault::query_size_failure ? NDIS_STATUS_FAILURE :
                                                              NDIS_STATUS_RESOURCES;
        return;
    }

    fixture.bad_contract |= *size != required_size || reinterpret_cast<PUCHAR>(list) != resource_storage;

    if (fixture.fault == Fault::query_fill_failure) {
        *status = NDIS_STATUS_FAILURE;
        return;
    }

    memset(list, 0, required_size);
    list->Version = 1;
    list->Revision = 1;
    list->Count = fixture.fault == Fault::invalid_count ? UINT32_MAX : 2;
    auto& memory = list->PartialDescriptors[0];
    memory.Type = fixture.fault == Fault::wrong_resource ? CmResourceTypePort : CmResourceTypeMemory;
    memory.u.Memory.Start.LowPart = 0x23450000;
    memory.u.Memory.Start.HighPart = 1;
    memory.u.Memory.Length = fixture.fault == Fault::short_aperture ? 256 : sizeof(registers);
    auto& interrupt = list->PartialDescriptors[1];
    interrupt.Type = fixture.fault == Fault::absent_interrupt ? CmResourceTypeNull : CmResourceTypeInterrupt;
    interrupt.ShareDisposition = CmResourceShareShared;
    interrupt.u.Interrupt.Level = 5;
    interrupt.u.Interrupt.Vector = 0x35;
    interrupt.u.Interrupt.Affinity = 1;
    *size = fixture.fault == Fault::query_grew ? required_size + 1 : required_size;
    *status = NDIS_STATUS_SUCCESS;
}

NDIS_STATUS NTAPI mock_allocate(PVOID* address, UINT length, ULONG)
{
    ++fixture.allocations;
    fixture.bad_contract |= length > sizeof(resource_storage);
    *address = nullptr;

    if (fixture.fault == Fault::allocation_failure) {
        return NDIS_STATUS_RESOURCES;
    }

    if (fixture.fault != Fault::allocation_null) {
        *address = resource_storage;
    }

    return NDIS_STATUS_SUCCESS;
}

VOID NTAPI mock_free(PVOID address, UINT length, UINT flags)
{
    ++fixture.frees;
    fixture.bad_contract |= address != resource_storage || flags != 0 ||
                            length != 8 + 2 * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
}

NDIS_STATUS NTAPI mock_map(PVOID* address, NDIS_HANDLE handle,
                           NDIS_PHYSICAL_ADDRESS physical, UINT length)
{
    ++fixture.maps;
    fixture.bad_contract |= handle != adapter_handle || length != sizeof(registers);
    fixture.mapped_physical = physical;
    *address = nullptr;

    if (fixture.fault == Fault::map_failure) {
        return NDIS_STATUS_RESOURCES;
    }

    if (fixture.fault != Fault::map_success_null) {
        *address = registers;
    }

    return fixture.fault == Fault::map_failure_with_pointer ? NDIS_STATUS_FAILURE : NDIS_STATUS_SUCCESS;
}

VOID NTAPI mock_unmap(NDIS_HANDLE handle, PVOID address, UINT length)
{
    ++fixture.unmaps;
    fixture.bad_contract |= handle != adapter_handle || address != registers || length != sizeof(registers);
}

UCHAR NTAPI mock_read8(PUCHAR address)
{
    ++fixture.reads;
    return *address;
}

USHORT NTAPI mock_read16(PUSHORT address)
{
    ++fixture.reads;
    return *address;
}

ULONG NTAPI mock_read32(PULONG address)
{
    ++fixture.reads;
    return *address;
}

VOID NTAPI mock_write8(PUCHAR address, UCHAR value)
{
    ++fixture.writes;
    *address = value;
}

VOID NTAPI mock_write16(PUSHORT address, USHORT value)
{
    ++fixture.writes;
    *address = value;
}

VOID NTAPI mock_write32(PULONG address, ULONG value)
{
    ++fixture.writes;
    *address = value;
}

void reset_fixture(Fault fault) noexcept
{
    fixture = {};
    fixture.fault = fault;
    memset(registers, 0, sizeof(registers));
    put32(registers + rtl8125::tx_config_offset,
          fault == Fault::removed_chip ? UINT32_MAX :
          fault == Fault::unknown_chip ? 0 : 0x64100000);
}

} // namespace

// Bind exactly the production object's imported ABI names; declarations/types
// above come from the real SDK. No alternate DDK definitions are introduced.
#if defined(_WIN64)
#define RTL8125_IMPORT(name, bytes) "__imp_" #name
#else
#define RTL8125_IMPORT(name, bytes) "__imp__" #name "@" #bytes
#endif
#define RTL8125_MOCK(name, bytes, implementation) \
    decltype(&name) import_##name __asm__(RTL8125_IMPORT(name, bytes)) = implementation

extern "C" {
RTL8125_MOCK(NdisReadPciSlotInformation, 20, mock_read_pci);
RTL8125_MOCK(NdisMQueryAdapterResources, 16, mock_query_resources);
RTL8125_MOCK(NdisAllocateMemoryWithTag, 12, mock_allocate);
RTL8125_MOCK(NdisFreeMemory, 12, mock_free);
RTL8125_MOCK(NdisMMapIoSpace, 20, mock_map);
RTL8125_MOCK(NdisMUnmapIoSpace, 12, mock_unmap);
RTL8125_MOCK(READ_REGISTER_UCHAR, 4, mock_read8);
RTL8125_MOCK(READ_REGISTER_USHORT, 4, mock_read16);
RTL8125_MOCK(READ_REGISTER_ULONG, 4, mock_read32);
RTL8125_MOCK(WRITE_REGISTER_UCHAR, 8, mock_write8);
RTL8125_MOCK(WRITE_REGISTER_USHORT, 8, mock_write16);
RTL8125_MOCK(WRITE_REGISTER_ULONG, 8, mock_write32);
}

int main()
{
    using namespace rtl8125::reactos;
    const Fault failures_to_inject[]{
        Fault::short_config, Fault::wrong_device, Fault::decode_disabled,
        Fault::query_size_failure, Fault::query_size_short, Fault::query_size_huge,
        Fault::allocation_failure, Fault::allocation_null, Fault::query_fill_failure,
        Fault::query_grew, Fault::invalid_count, Fault::short_aperture,
        Fault::wrong_resource, Fault::absent_interrupt, Fault::map_failure,
        Fault::map_failure_with_pointer, Fault::map_success_null,
        Fault::removed_chip, Fault::unknown_chip
    };

    for (const auto fault : failures_to_inject) {
        reset_fixture(fault);
        DeviceResources resources{};
        expect(acquire_resources(adapter_handle, configuration_context, resources) != NDIS_STATUS_SUCCESS,
               "injected initialization failure propagated");
        expect(resources.mapped_address == nullptr && resources.mapped_length == 0 &&
               resources.chip.status == rtl8125::ChipStatus::unknown_mac,
               "failed acquisition leaves truthful empty state");
        expect(fixture.frees == (fixture.queries >= 2 ? 1U : 0U), "every resource-list allocation retired");
        const auto expected_unmaps = fault == Fault::removed_chip || fault == Fault::unknown_chip ||
                                     fault == Fault::map_failure_with_pointer ? 1U : 0U;
        expect(fixture.unmaps == expected_unmaps, "all partial mappings retired exactly once");
        expect(!fixture.bad_contract && fixture.writes == 0, "exact NDIS handles/lengths; probe never writes");
        release_resources(adapter_handle, resources);
        expect(fixture.unmaps == expected_unmaps, "release after failure is idempotent");
    }

    reset_fixture(Fault::none);
    DeviceResources resources{};
    expect(acquire_resources(adapter_handle, configuration_context, resources) == NDIS_STATUS_SUCCESS,
           "successful read-only native acquisition");
    expect(resources.chip.version == rtl8125::MacVersion::rtl8125b &&
           resources.chip.xid == 0x641 && resources.tx_config == 0x64100000,
           "TxConfig probe selects B XID from owned mapping");
    expect(fixture.mapped_physical.HighPart == 1 && fixture.mapped_physical.LowPart == 0x23450000 &&
           resources.physical_address.HighPart == 1, "physical address above 4 GiB survives i386 boundary");
    expect(fixture.maps == 1 && fixture.frees == 1 && fixture.reads == 1 && fixture.writes == 0,
           "acquire maps only assigned aperture, frees query buffer, reads only XID");
    expect(acquire_resources(adapter_handle, configuration_context, resources) != NDIS_STATUS_SUCCESS &&
           resources.mapped_address == registers && fixture.maps == 1,
           "reacquire cannot overwrite an owned mapping");

    UCHAR byte = 99;
    USHORT word = 99;
    ULONG dword = 99;
    const auto reads_before = fixture.reads;
    expect(!read_mmio8(resources, sizeof(registers), byte) && byte == 0, "byte read at end rejected and cleared");
    expect(!read_mmio16(resources, 1, word) && word == 0, "unaligned word read rejected and cleared");
    expect(!read_mmio32(resources, UINT32_MAX, dword) && dword == 0, "wrapping dword read rejected and cleared");
    expect(!write_mmio8(resources, sizeof(registers), 1) && !write_mmio16(resources, 1, 1) &&
           !write_mmio32(resources, UINT32_MAX, 1) && fixture.writes == 0 &&
           fixture.reads == reads_before, "invalid typed accesses perform no register calls");
    expect(write_mmio8(resources, 0x100, 0x12) && read_mmio8(resources, 0x100, byte) && byte == 0x12,
           "valid byte accessor roundtrip against mapped fixture");
    expect(write_mmio16(resources, 0x102, 0x3456) && read_mmio16(resources, 0x102, word) && word == 0x3456,
           "valid word accessor roundtrip against mapped fixture");
    expect(write_mmio32(resources, sizeof(registers) - 4, 0x789abcde) &&
           read_mmio32(resources, sizeof(registers) - 4, dword) && dword == 0x789abcde,
           "valid final dword accessor roundtrip");
    expect(flush_posted_writes(resources), "posted-write flush reads ChipCmd");
    registers[0x37] = 0xff;
    expect(!flush_posted_writes(resources), "flush detects removed-device all-ones read");
    release_resources(adapter_handle, resources);
    release_resources(adapter_handle, resources);
    expect(fixture.unmaps == 1 && resources.mapped_address == nullptr && !fixture.bad_contract,
           "successful mapping released once; second release is empty");
    expect(!read_mmio32(resources, 0, dword) && !write_mmio32(resources, 0, 1),
           "access after release fails closed");
    expect(acquire_resources(nullptr, configuration_context, resources) != NDIS_STATUS_SUCCESS &&
           acquire_resources(adapter_handle, nullptr, resources) != NDIS_STATUS_SUCCESS,
           "null native handles rejected");
    printf("RTL8125 real-SDK resource contracts: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
