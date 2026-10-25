// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.miniport_contract
Purpose: Exercise production callbacks using genuine SDK ABI and injected imports.
Created: 2026-10-24
Architecture: Windows user-mode fixture; not a kernel/ReactOS guest test.
*/
#include "../src/reactos/adapter.hpp"

#include <new>
#include <cstdio>

extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);

namespace {

using rtl8125::reactos::Adapter;

struct Fixture {
    unsigned checks{};
    unsigned failures{};
    unsigned allocations{};
    unsigned frees{};
    unsigned acquisitions{};
    unsigned releases{};
    unsigned registrations{};
    unsigned terminations{};
    bool wrapper_available{true};
    bool allocation_failure{};
    bool extension_failure{};
    NDIS_STATUS registration_status{NDIS_STATUS_SUCCESS};
    NDIS_STATUS probe_status{NDIS_STATUS_SUCCESS};
    KIRQL irql{PASSIVE_LEVEL};
    NDIS_MINIPORT_CHARACTERISTICS table{};
    ULONG attributes{};
    PDRIVER_UNLOAD unload{};
    PVOID extension_key{};
    alignas(NDIS_HANDLE) unsigned char extension[64]{};
    alignas(Adapter) unsigned char storage[sizeof(Adapter)]{};

    void expect(bool condition, const char* message) noexcept
    {
        ++checks;

        if (!condition) {
            ++failures;
            std::printf("FAIL: %s\n", message);
        }
    }
};

// Mock imports have the OS ABI and cannot receive a test context. One explicit
// fixture owner supplies their state; production driver has no mutable globals.
Fixture* fixture{};

NDIS_STATUS NTAPI mock_allocate(PVOID* output, UINT length, ULONG)
{
    *output = nullptr;

    if (fixture->allocation_failure || length != sizeof(Adapter)) {
        return NDIS_STATUS_RESOURCES;
    }

    ++fixture->allocations;
    *output = fixture->storage;
    return NDIS_STATUS_SUCCESS;
}

VOID NTAPI mock_free(PVOID pointer, UINT length, UINT flags)
{
    fixture->expect(pointer == fixture->storage && length == sizeof(Adapter) && flags == 0,
                    "Free uses original adapter storage and exact allocation size");
    ++fixture->frees;
}

VOID NTAPI mock_initialize(PNDIS_HANDLE wrapper, PVOID, PVOID, PVOID)
{
    *wrapper = fixture->wrapper_available ? fixture : nullptr;
}

VOID NTAPI mock_terminate(NDIS_HANDLE wrapper, PVOID)
{
    fixture->expect(wrapper == fixture, "Terminate uses acquired wrapper");
    ++fixture->terminations;
}

NDIS_STATUS NTAPI mock_register(NDIS_HANDLE wrapper,
                                PNDIS_MINIPORT_CHARACTERISTICS table, UINT length)
{
    fixture->expect(wrapper == fixture && length == sizeof(*table),
                    "Registration uses full genuine NDIS 5.1 table layout");
    fixture->expect(table->Ndis50Chars.Ndis40Chars.Ndis30Chars.ISRHandler != nullptr &&
        table->Ndis50Chars.Ndis40Chars.Ndis30Chars.HandleInterruptHandler != nullptr,
        "Actual production registration includes native ISR and bounded handler");
    fixture->table = *table;
    ++fixture->registrations;
    return fixture->registration_status;
}

VOID NTAPI mock_attributes(NDIS_HANDLE, NDIS_HANDLE context, UINT hang_interval,
                           ULONG flags, NDIS_INTERFACE_TYPE type)
{
    fixture->expect(context == fixture->storage && hang_interval == 0 &&
                    type == NdisInterfacePci, "Adapter attributes carry owned state and PCI bus");
    fixture->attributes = flags;
}

KIRQL NTAPI mock_irql()
{
    return fixture->irql;
}

NTSTATUS NTAPI mock_extension_allocate(PDRIVER_OBJECT, PVOID key, ULONG size, PVOID* output)
{
    *output = nullptr;

    if (fixture->extension_failure || size > sizeof(fixture->extension)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fixture->extension_key = key;
    *output = fixture->extension;
    return STATUS_SUCCESS;
}

PVOID NTAPI mock_extension_get(PDRIVER_OBJECT, PVOID key)
{
    fixture->expect(key == fixture->extension_key, "Unload retrieves its owned driver extension");
    return fixture->extension;
}

VOID NTAPI mock_register_unload(NDIS_HANDLE wrapper, PDRIVER_UNLOAD unload)
{
    fixture->expect(wrapper == fixture && unload != nullptr,
                    "Successful NDIS registration installs explicit wrapper cleanup");
    fixture->unload = unload;
}

} // namespace

// Import pointers retain the same symbol decoration and exact declared SDK
// function pointer types used by the production object. No replacement headers.
extern "C" {
decltype(&NdisAllocateMemoryWithTag) allocate_import __asm__("__imp__NdisAllocateMemoryWithTag@12") = mock_allocate;
decltype(&NdisFreeMemory) free_import __asm__("__imp__NdisFreeMemory@12") = mock_free;
decltype(&NdisInitializeWrapper) initialize_import __asm__("__imp__NdisInitializeWrapper@16") = mock_initialize;
decltype(&NdisTerminateWrapper) terminate_import __asm__("__imp__NdisTerminateWrapper@8") = mock_terminate;
decltype(&NdisMRegisterMiniport) register_import __asm__("__imp__NdisMRegisterMiniport@12") = mock_register;
decltype(&NdisMSetAttributesEx) attributes_import __asm__("__imp__NdisMSetAttributesEx@20") = mock_attributes;
decltype(&KeGetCurrentIrql) irql_import __asm__("__imp__KeGetCurrentIrql@0") = mock_irql;
decltype(&IoAllocateDriverObjectExtension) extension_allocate_import __asm__("__imp__IoAllocateDriverObjectExtension@16") = mock_extension_allocate;
decltype(&IoGetDriverObjectExtension) extension_get_import __asm__("__imp__IoGetDriverObjectExtension@8") = mock_extension_get;
decltype(&NdisMRegisterUnloadHandler) unload_import __asm__("__imp__NdisMRegisterUnloadHandler@8") = mock_register_unload;
ULONG __cdecl fixture_debug(PCSTR, ...) __asm__("_DbgPrint");
ULONG __cdecl fixture_debug(PCSTR, ...)
{
    return 0;
}
}

namespace rtl8125::reactos {

NDIS_STATUS acquire_resources(NDIS_HANDLE adapter, NDIS_HANDLE configuration,
                              DeviceResources& resources) noexcept
{
    fixture->expect(adapter == fixture && configuration == fixture,
                    "Probe receives distinct native handle roles unchanged");
    ++fixture->acquisitions;

    if (fixture->probe_status == NDIS_STATUS_SUCCESS) {
        resources.mapped_address = fixture->storage;
        resources.mapped_length = 0x10000;
        resources.chip = identify_chip(realtek_vendor_id, rtl8125_device_id, 0x64100000);
    }

    return fixture->probe_status;
}

void release_resources(NDIS_HANDLE handle, DeviceResources& resources) noexcept
{
    fixture->expect(handle == fixture, "Resource cleanup retains adapter handle");
    ++fixture->releases;
    resources = {};
}

} // namespace rtl8125::reactos

int main()
{
    Fixture state{};
    fixture = &state;
    state.wrapper_available = false;
    state.expect(DriverEntry(nullptr, nullptr) == STATUS_INSUFFICIENT_RESOURCES &&
                 state.registrations == 0 && state.terminations == 0,
                 "Wrapper failure never registers or terminates an unowned wrapper");
    state.wrapper_available = true;
    state.extension_failure = true;
    state.expect(DriverEntry(nullptr, nullptr) == STATUS_INSUFFICIENT_RESOURCES &&
                 state.terminations == 1 && state.registrations == 0,
                 "Driver extension allocation failure terminates acquired wrapper");
    state.extension_failure = false;
    state.registration_status = NDIS_STATUS_FAILURE;
    state.expect(DriverEntry(nullptr, nullptr) == static_cast<NTSTATUS>(NDIS_STATUS_FAILURE) &&
                 state.terminations == 2, "Failed registration terminates wrapper once");
    state.registration_status = NDIS_STATUS_SUCCESS;
    state.expect(DriverEntry(nullptr, nullptr) == STATUS_SUCCESS && state.terminations == 2 &&
                 state.unload != nullptr,
                 "Successful registration retains wrapper in driver-owned unload state");
    const auto& callbacks = state.table.Ndis50Chars.Ndis40Chars.Ndis30Chars;
    state.expect(callbacks.MajorNdisVersion == 5 && callbacks.MinorNdisVersion == 1 &&
                 callbacks.InitializeHandler != nullptr && callbacks.HaltHandler != nullptr &&
                 callbacks.QueryInformationHandler != nullptr &&
                 callbacks.SetInformationHandler != nullptr && callbacks.SendHandler != nullptr &&
                 callbacks.ResetHandler != nullptr && callbacks.ISRHandler != nullptr &&
                 callbacks.HandleInterruptHandler != nullptr &&
                 state.table.PnPEventNotifyHandler != nullptr &&
                 state.table.AdapterShutdownHandler != nullptr,
                 "Correct 5.1 callback layout with native ISR and bounded packet DPC");

    NDIS_STATUS open_error{};
    UINT selected = 99;
    NDIS_MEDIUM media[]{NdisMediumWan, NdisMedium802_3};
    state.expect(callbacks.InitializeHandler(&open_error, &selected, media, 1, &state, &state) ==
                 NDIS_STATUS_UNSUPPORTED_MEDIA && state.allocations == 0,
                 "Non-Ethernet initialize fails without acquiring adapter state");
    state.expect(callbacks.InitializeHandler(&open_error, &selected, nullptr, 2, &state, &state) ==
                 NDIS_STATUS_FAILURE && state.allocations == 0,
                 "Invalid media pointer fails before dereference");
    state.irql = DISPATCH_LEVEL;
    state.expect(callbacks.InitializeHandler(&open_error, &selected, media, 2, &state, &state) ==
                 NDIS_STATUS_FAILURE && state.allocations == 0,
                 "Wrong initialize IRQL cannot acquire resources");
    state.irql = PASSIVE_LEVEL;
    state.allocation_failure = true;
    state.expect(callbacks.InitializeHandler(&open_error, &selected, media, 2, &state, &state) ==
                 NDIS_STATUS_RESOURCES && state.acquisitions == 0 && state.frees == 0,
                 "Allocation failure performs no probe and frees no unowned memory");
    state.allocation_failure = false;
    state.probe_status = NDIS_STATUS_ADAPTER_NOT_FOUND;
    state.expect(callbacks.InitializeHandler(&open_error, &selected, media, 2, &state, &state) ==
                 NDIS_STATUS_ADAPTER_NOT_FOUND && selected == 1 &&
                 state.allocations == 1 && state.frees == 1 && state.releases == 1,
                 "Rejected hardware unwinds owned context and leaves no adapter success");
    state.probe_status = NDIS_STATUS_SUCCESS;
    state.expect(callbacks.InitializeHandler(&open_error, &selected, media, 2, &state, &state) ==
                 NDIS_STATUS_NOT_SUPPORTED && open_error == NDIS_STATUS_NOT_SUPPORTED &&
                 state.allocations == 2 && state.frees == 2 && state.releases == 2 &&
                 state.attributes == 0,
                 "Successful read-only chip probe still fails closed and releases every resource");

    auto* adapter = new (state.storage) Adapter{};
    adapter->handle = &state;
    ULONG written = 99;
    ULONG needed = 99;
    ULONG scalar = 99;
    state.expect(callbacks.QueryInformationHandler(adapter, OID_GEN_LINK_SPEED, &scalar,
                 sizeof(scalar), &written, &needed) == NDIS_STATUS_SUCCESS && scalar == 0 &&
                 written == sizeof(scalar) && needed == 0, "Link speed is zero before a datapath");
    state.expect(callbacks.QueryInformationHandler(adapter, OID_GEN_MEDIA_CONNECT_STATUS, &scalar,
                 sizeof(scalar), &written, &needed) == NDIS_STATUS_SUCCESS &&
                 scalar == NdisMediaStateDisconnected, "Probe cannot advertise connected media");
    state.expect(callbacks.QueryInformationHandler(adapter, OID_802_3_CURRENT_ADDRESS, &scalar,
                 sizeof(scalar), &written, &needed) == NDIS_STATUS_ADAPTER_NOT_READY &&
                 written == 0 && needed == 0, "Unvalidated MAC address is never fabricated");
    NDIS_OID oids[64]{};
    state.expect(callbacks.QueryInformationHandler(adapter, OID_GEN_SUPPORTED_LIST, oids,
                 sizeof(oids), &written, &needed) == NDIS_STATUS_SUCCESS &&
                 written != 0 && written % sizeof(NDIS_OID) == 0,
                 "Supported OIDs are actual native SDK identities");
    const auto oid_count = written / sizeof(NDIS_OID);

    for (ULONG index = 0; index < oid_count; ++index) {
        unsigned char bytes[256]{};
        const auto status = callbacks.QueryInformationHandler(adapter, oids[index], bytes,
                                                               sizeof(bytes), &written, &needed);
        state.expect(status == NDIS_STATUS_SUCCESS ||
                     ((oids[index] == OID_802_3_CURRENT_ADDRESS ||
                       oids[index] == OID_802_3_PERMANENT_ADDRESS) &&
                      status == NDIS_STATUS_ADAPTER_NOT_READY),
                     "Every advertised OID dispatches to a defined truthful response");
    }

    unsigned char guards[]{0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};

    for (ULONG capacity = 0; capacity < sizeof(ULONG); ++capacity) {
        state.expect(callbacks.QueryInformationHandler(adapter, OID_GEN_LINK_SPEED, guards + 1,
                     capacity, &written, &needed) == NDIS_STATUS_INVALID_LENGTH &&
                     written == 0 && needed == sizeof(ULONG) && guards[0] == 0xcc &&
                     guards[1] == 0xcc && guards[5] == 0xcc,
                     "Native short query initializes counts and leaves bytes untouched");
    }

    state.expect(callbacks.QueryInformationHandler(adapter, 0xdeadbeef, &scalar, sizeof(scalar),
                 &written, &needed) == NDIS_STATUS_NOT_SUPPORTED && written == 0 && needed == 0,
                 "Unknown query has deterministic unsupported status and output counts");
    state.expect(callbacks.QueryInformationHandler(adapter, OID_GEN_LINK_SPEED, nullptr,
                 sizeof(scalar), &written, &needed) == NDIS_STATUS_INVALID_DATA && written == 0,
                 "Nonempty null native query output fails safely");
    constexpr NDIS_OID counter_oids[]{OID_GEN_XMIT_OK, OID_GEN_RCV_OK,
        OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER};

    for (const auto oid : counter_oids) {
        for (ULONG capacity = 0; capacity <= 10; ++capacity) {
            unsigned char bytes[12];

            for (auto& byte : bytes) {
                byte = 0xcc;
            }

            const auto status = callbacks.QueryInformationHandler(adapter, oid, bytes + 1,
                                                                   capacity, &written, &needed);
            const auto expected_written = capacity < 4 ? 0u : (capacity < 8 ? 4u : 8u);
            state.expect(status == (capacity < 4 ? NDIS_STATUS_INVALID_LENGTH :
                                                   NDIS_STATUS_SUCCESS) &&
                         written == expected_written && needed == 8,
                         "Native general statistics negotiate32/64 without reporting short success");
            bool exact_bytes = bytes[0] == 0xcc;

            for (ULONG index = 1; index < sizeof(bytes); ++index) {
                exact_bytes = exact_bytes && bytes[index] ==
                    (index <= expected_written ? 0 : 0xcc);
            }

            state.expect(exact_bytes, "Native statistic query preserves output guards and tail");
        }

        state.expect(callbacks.QueryInformationHandler(adapter, oid, nullptr, 8,
                     &written, &needed) == NDIS_STATUS_INVALID_DATA && written == 0,
                     "Native counter query refuses null storage without dereference");
    }

    scalar = 1;
    state.expect(callbacks.SetInformationHandler(adapter, OID_GEN_CURRENT_PACKET_FILTER, &scalar,
                 sizeof(scalar), &written, &needed) == NDIS_STATUS_NOT_SUPPORTED &&
                 written == 0 && adapter->packet_filter == 0,
                 "Nonzero filter is not acknowledged without hardware implementation");
    scalar = 0;
    state.expect(callbacks.SetInformationHandler(adapter, OID_GEN_CURRENT_PACKET_FILTER, &scalar,
                 sizeof(scalar), &written, &needed) == NDIS_STATUS_SUCCESS && written == 4 &&
                 needed == 0, "Empty filter round trip has exact byte counts");
    scalar = 1501;
    state.expect(callbacks.SetInformationHandler(adapter, OID_GEN_CURRENT_LOOKAHEAD, &scalar,
                 sizeof(scalar), &written, &needed) == NDIS_STATUS_INVALID_DATA &&
                 adapter->lookahead == 1500 && written == 0, "Native lookahead enforces MTU");
    state.expect(callbacks.SetInformationHandler(adapter, OID_GEN_CURRENT_LOOKAHEAD, &scalar,
                 3, &written, &needed) == NDIS_STATUS_INVALID_LENGTH &&
                 written == 0 && needed == 4, "Native scalar setter reports exact input size");
    state.expect(callbacks.SetInformationHandler(adapter, OID_802_3_MULTICAST_LIST, nullptr,
                 0, &written, &needed) == NDIS_STATUS_SUCCESS && written == 0,
                 "Only empty multicast state succeeds before filter implementation");
    state.expect(callbacks.SendHandler(adapter, nullptr, 0) == NDIS_STATUS_ADAPTER_NOT_READY,
                 "Send never accepts or pends ownership of packets");
    BOOLEAN addressing_reset = TRUE;
    state.expect(callbacks.ResetHandler(&addressing_reset, adapter) == NDIS_STATUS_NOT_SUPPORTED &&
                 addressing_reset == FALSE, "Reset cannot claim recovery or addressing reset");
    callbacks.HaltHandler(adapter);
    state.expect(state.frees == 3 && state.releases == 3,
                 "Halt releases independently constructed owned adapter exactly once");
    callbacks.HaltHandler(nullptr);
    state.expect(state.frees == 3, "Null halt does not release unowned storage");
    state.unload(nullptr);
    state.expect(state.terminations == 3, "Registered unload terminates wrapper exactly once");
    state.unload(nullptr);
    state.expect(state.terminations == 3, "Repeated empty unload cannot terminate wrapper twice");
    std::printf("Native SDK miniport contract checks: %u passed, %u failed\n",
                state.checks - state.failures, state.failures);
    fixture = nullptr;
    return state.failures == 0 ? 0 : 1;
}
