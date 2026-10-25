// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.interrupt_native
Purpose: Execute genuine-SDK ISR, private scheduler and lifetime boundaries.
Created: 2026-10-24
Architecture: Windows user-mode imported-API fixtures; no physical interrupt proof.
*/
#include "../src/reactos/adapter.hpp"
#include <stdio.h>
#include <string.h>
#include <new>

extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);
namespace {
using namespace rtl8125;
using namespace rtl8125::reactos;
struct Fixture {
    KIRQL irql{};
    ULONG status{}, mask{}, tx_budget{}, rx_budget{}, tx_calls{}, rx_calls{}, link_calls{};
    unsigned checks{}, failures{}, writes{}, flushes{}, registered{}, deregistered{}, synced{}, indicated{}, sleeps{};
    unsigned order_count{};
    char order[256]{};
    bool fail_register{}, fail_write{}, fail_flush{}, fail_schedule{}, fail_link{}, fail_read{}, inject_enable{}, busy{}, force_wait{};
    bool shared{}, isr{}, bad{};
    NDIS_INTERRUPT_MODE mode{};
    UINT vector{}, level{};
    PKDPC queued{};
    PNDIS_WORK_ITEM work{};
    Adapter* adapter{};
} f;
NDIS_MINIPORT_BLOCK miniport{};
NDIS_MINIPORT_CHARACTERISTICS callbacks{};
alignas(void*) UCHAR extension[64]{};
PVOID extension_key{};
NDIS_WORK_ITEM test_link_work{};
bool dispatch_link(Adapter&) noexcept;
alignas(Adapter) unsigned char storage[sizeof(Adapter)];
void expect(bool value, const char* message)
{
    ++f.checks;
    if (!value) { ++f.failures; printf("FAIL: %s\n", message); }
}
void order(char value)
{
    if (f.order_count < sizeof(f.order)) { f.order[f.order_count++] = value; }
}
VOID NTAPI status(NDIS_HANDLE, NDIS_STATUS value, PVOID, UINT)
{
    expect(value == NDIS_STATUS_MEDIA_CONNECT || value == NDIS_STATUS_MEDIA_DISCONNECT,
        "link status is an actual media transition");
    ++f.indicated;
}
VOID NTAPI status_complete(NDIS_HANDLE) {}
Adapter& fresh()
{
    const auto checks = f.checks, failures = f.failures;
    f = {};
    f.checks = checks; f.failures = failures;
    memset(&miniport, 0, sizeof(miniport));
    miniport.StatusHandler = status;
    miniport.StatusCompleteHandler = status_complete;
    auto& adapter = *new (storage) Adapter{};
    adapter.handle = &miniport;
    adapter.resources.mapped_address = reinterpret_cast<PUCHAR>(0x10000);
    adapter.resources.mapped_length = 65536;
    adapter.resources.chip = {ChipStatus::supported, MacVersion::rtl8125b, 0x641};
    adapter.resources.interrupt_vector = 11;
    adapter.resources.interrupt_level = 11;
    adapter.resources.interrupt_affinity = 1;
    adapter.resources.interrupt_shared = TRUE;
    adapter.hardware.device.version = MacVersion::rtl8125b;
    adapter.hardware.device.initialized = true;
    adapter.hardware.device.running = true;
    adapter.phase = AdapterPhase::running;
    adapter.interrupts.schedule_link = dispatch_link;
    adapter.datapath.running = true;
    adapter.packet_prerequisites.retire_cpu_access = [](void*) noexcept { return true; };
    f.adapter = &adapter;
    return adapter;
}
KIRQL NTAPI irql() { return f.irql; }
KIRQL FASTCALL raise_irql(KIRQL value) { const auto old = f.irql; f.irql = value; return old; }
VOID FASTCALL lower_irql(KIRQL value) { f.irql = value; }
VOID NTAPI init_dpc(PKDPC dpc, PKDEFERRED_ROUTINE routine, PVOID context)
{
    memset(dpc, 0, sizeof(*dpc)); dpc->DeferredRoutine = routine; dpc->DeferredContext = context;
}
BOOLEAN NTAPI insert_dpc(PKDPC dpc, PVOID, PVOID)
{
    if (f.queued != nullptr) { expect(f.queued == dpc, "only one private continuation object exists"); return FALSE; }
    f.queued = dpc;
    return TRUE;
}
BOOLEAN NTAPI remove_dpc(PKDPC dpc)
{
    if (f.queued != dpc) { return FALSE; }
    f.queued = nullptr;
    return TRUE;
}
BOOLEAN NTAPI synchronize(PNDIS_MINIPORT_INTERRUPT, PVOID function, PVOID context)
{
    ++f.synced;
    const auto old = f.irql; f.irql = 5;
    const auto result = reinterpret_cast<PKSYNCHRONIZE_ROUTINE>(function)(context);
    f.irql = old;
    return result;
}
NDIS_STATUS NTAPI register_irq(PNDIS_MINIPORT_INTERRUPT, NDIS_HANDLE handle, UINT vector,
    UINT level, BOOLEAN request, BOOLEAN shared, NDIS_INTERRUPT_MODE mode)
{
    ++f.registered;
    f.vector = vector; f.level = level; f.isr = request; f.shared = shared; f.mode = mode;
    expect(handle == &miniport && f.mask == 0, "line is masked before NDIS registration");
    BOOLEAN ours = TRUE, wrapper = TRUE;
    callbacks.Ndis50Chars.Ndis40Chars.Ndis30Chars.ISRHandler(&ours, &wrapper, f.adapter);
    expect(!ours && !wrapper && f.queued == nullptr, "initialization ISR cannot schedule before enable");
    return f.fail_register ? NDIS_STATUS_RESOURCE_CONFLICT : NDIS_STATUS_SUCCESS;
}
VOID NTAPI deregister_irq(PNDIS_MINIPORT_INTERRUPT)
{
    expect(f.adapter->interrupts.references == 0 && f.queued == nullptr && f.work == nullptr,
        "all tracked callback objects drain before disconnect");
    ++f.deregistered;
}
NDIS_STATUS NTAPI schedule(PNDIS_WORK_ITEM work)
{
    if (f.fail_schedule) { return NDIS_STATUS_RESOURCES; }
    expect(f.work == nullptr, "same link work item is never queued twice");
    f.work = work;
    return NDIS_STATUS_SUCCESS;
}
VOID NTAPI link_thunk(PNDIS_WORK_ITEM, PVOID context)
{ service_interrupt_link(*static_cast<Adapter*>(context)); }
bool dispatch_link(Adapter& adapter) noexcept
{
    NdisInitializeWorkItem(&test_link_work, link_thunk, &adapter);
    return NdisScheduleWorkItem(&test_link_work) == NDIS_STATUS_SUCCESS;
}
void run_work()
{
    auto* work = f.work;
    f.work = nullptr;
    if (work != nullptr) {
        const auto old = f.irql; f.irql = PASSIVE_LEVEL;
        work->Routine(work, work->Context); f.irql = old;
    }
}
void run_dpc()
{
    auto* dpc = f.queued;
    f.queued = nullptr;
    if (dpc != nullptr) {
        const auto old = f.irql; f.irql = DISPATCH_LEVEL;
        dpc->DeferredRoutine(dpc, dpc->DeferredContext, nullptr, nullptr); f.irql = old;
    }
}
VOID NTAPI sleep(ULONG value)
{
    ++f.sleeps;
    if (value != 1000) { expect(false, "retirement wait has bounded millisecond intervals"); }
    if (!f.force_wait) { run_work(); run_dpc(); }
}
VOID NTAPI wrapper(PNDIS_HANDLE value, PVOID, PVOID, PVOID) { *value = &miniport; }
VOID NTAPI terminate(NDIS_HANDLE, PVOID) {}
NDIS_STATUS NTAPI register_miniport(NDIS_HANDLE, PNDIS_MINIPORT_CHARACTERISTICS value, UINT size)
{
    expect(size == sizeof(*value), "real DriverEntry submits genuine full characteristics");
    callbacks = *value;
    return NDIS_STATUS_SUCCESS;
}
NTSTATUS NTAPI allocate_extension(PDRIVER_OBJECT, PVOID key, ULONG size, PVOID* output)
{
    extension_key = key;
    *output = size <= sizeof(extension) ? extension : nullptr;
    return *output != nullptr ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
PVOID NTAPI get_extension(PDRIVER_OBJECT, PVOID key) { return key == extension_key ? extension : nullptr; }
VOID NTAPI register_unload(NDIS_HANDLE, PDRIVER_UNLOAD) {}
VOID NTAPI attributes(NDIS_HANDLE, NDIS_HANDLE, UINT, ULONG, NDIS_INTERFACE_TYPE) {}
NDIS_STATUS NTAPI allocate_memory(PVOID* output, UINT, ULONG) { *output = nullptr; return NDIS_STATUS_RESOURCES; }
VOID NTAPI free_memory(PVOID, UINT, UINT) {}
void start(Adapter& adapter)
{
    expect(register_interrupt(adapter) == NDIS_STATUS_SUCCESS, "assigned supported line registers");
    expect(enable_interrupt(adapter) == NDIS_STATUS_SUCCESS, "actual running hardware enables service");
    run_dpc(); run_work(); run_dpc();
    expect(f.mask == interrupt_event_mask && adapter.interrupts.references == 0,
        "initial packet/link work drains and arms hardware events");
    f.tx_calls = f.rx_calls = f.link_calls = 0;
    f.order_count = 0;
}
void fire(Adapter& adapter, ULONG value, bool expected)
{
    f.status = value;
    BOOLEAN ours = TRUE, wrapper = TRUE;
    const auto old = f.irql; f.irql = 5;
    callbacks.Ndis50Chars.Ndis40Chars.Ndis30Chars.ISRHandler(&ours, &wrapper, &adapter); f.irql = old;
    expect((ours != FALSE) == expected && wrapper == FALSE,
        "ISR reports only its events and never queues untracked wrapper DPC");
}
}
namespace rtl8125::reactos {
bool read_mmio32(const DeviceResources&, ULONG offset, ULONG& value) noexcept
{
    expect(offset == 0x3c, "IRQ reads exact 32-bit RTL8125 status register");
    value = f.status;
    order('R');
    return !f.fail_read;
}
bool write_mmio32(const DeviceResources&, ULONG offset, ULONG value) noexcept
{
    ++f.writes;
    expect(offset == 0x38 || offset == 0x3c, "IRQ writes only mask or status register");
    if (f.fail_write) { return false; }
    if (offset == 0x38) {
        f.mask = value; order(value == 0 ? 'M' : 'E');
        if (value != 0 && f.inject_enable) { f.status |= 1; f.inject_enable = false; }
    } else {
        expect((value & ~interrupt_event_mask) == 0, "W1C acknowledges observed known events only");
        f.status &= ~value; order('A');
    }
    return true;
}
bool flush_posted_writes(const DeviceResources&) noexcept { ++f.flushes; order('F'); return !f.fail_flush; }
ULONG poll_transmit(Datapath&, ULONG budget) noexcept
{
    expect(budget == interrupt_packet_budget, "TX DPC budget is fixed and bounded");
    ++f.tx_calls;
    const auto value = f.tx_budget; f.tx_budget = 0;
    return value;
}
ULONG poll_receive(Datapath&, ULONG budget) noexcept
{
    expect(budget == interrupt_packet_budget, "RX DPC budget is fixed and bounded");
    ++f.rx_calls;
    const auto value = f.rx_budget; f.rx_budget = 0;
    return value;
}
void stop_datapath(Datapath& datapath) noexcept { datapath.running = false; }
NDIS_STATUS acquire_resources(NDIS_HANDLE, NDIS_HANDLE, DeviceResources&) noexcept { return NDIS_STATUS_FAILURE; }
void release_resources(NDIS_HANDLE, DeviceResources&) noexcept {}
NDIS_STATUS stop_packet_path(Adapter&) noexcept { return NDIS_STATUS_SUCCESS; }
NDIS_STATUS send_packet(Datapath&, PNDIS_PACKET) noexcept { return NDIS_STATUS_ADAPTER_NOT_READY; }
NDIS_STATUS query_datapath_information(const Datapath&, NDIS_OID, PVOID, ULONG, PULONG, PULONG) noexcept
{ return NDIS_STATUS_NOT_SUPPORTED; }
NDIS_STATUS set_datapath_information(Datapath&, NDIS_OID, PVOID, ULONG, PULONG, PULONG) noexcept
{ return NDIS_STATUS_NOT_SUPPORTED; }
NDIS_STATUS query_hardware_link(HardwareAccess&, const HardwareState&, hardware::LinkState& output) noexcept
{
    expect(f.irql == PASSIVE_LEVEL, "PHY work uses real PASSIVE-level boundary");
    ++f.link_calls;
    output = {}; output.up = true; output.full_duplex = true; output.autoneg_complete = true; output.speed_mbps = 1000;
    return f.fail_link ? NDIS_STATUS_FAILURE : NDIS_STATUS_SUCCESS;
}
bool report_packet_link(Adapter& adapter, const hardware::LinkState& link) noexcept
{
    adapter.link_up = link.up;
    adapter.link_speed_100bps = link.speed_mbps * 10000;
    return true;
}
}
#define MOCK(name, bytes, implementation) \
    decltype(&name) import_##name __asm__("__imp__" #name "@" #bytes) = implementation
extern "C" {
MOCK(KeGetCurrentIrql, 0, irql);
MOCK(KeInitializeDpc, 12, init_dpc);
MOCK(KeInsertQueueDpc, 12, insert_dpc);
MOCK(KeRemoveQueueDpc, 4, remove_dpc);
MOCK(NdisMRegisterInterrupt, 28, register_irq);
MOCK(NdisMDeregisterInterrupt, 4, deregister_irq);
MOCK(NdisMSynchronizeWithInterrupt, 12, synchronize);
MOCK(NdisScheduleWorkItem, 4, schedule);
MOCK(NdisMSleep, 4, sleep);
decltype(&KfRaiseIrql) import_raise __asm__("__imp_@KfRaiseIrql@4") = raise_irql;
decltype(&KfLowerIrql) import_lower __asm__("__imp_@KfLowerIrql@4") = lower_irql;
MOCK(NdisInitializeWrapper, 16, wrapper);
MOCK(NdisTerminateWrapper, 8, terminate);
MOCK(NdisMRegisterMiniport, 12, register_miniport);
MOCK(NdisMRegisterUnloadHandler, 8, register_unload);
MOCK(IoAllocateDriverObjectExtension, 16, allocate_extension);
MOCK(IoGetDriverObjectExtension, 8, get_extension);
MOCK(NdisMSetAttributesEx, 20, attributes);
MOCK(NdisAllocateMemoryWithTag, 12, allocate_memory);
MOCK(NdisFreeMemory, 12, free_memory);
ULONG __cdecl debug(PCSTR, ...) __asm__("_DbgPrint");
ULONG __cdecl debug(PCSTR, ...) { return 0; }
}
int main()
{
    expect(DriverEntry(nullptr, nullptr) == STATUS_SUCCESS &&
        callbacks.Ndis50Chars.Ndis40Chars.Ndis30Chars.ISRHandler != nullptr &&
        callbacks.Ndis50Chars.Ndis40Chars.Ndis30Chars.HandleInterruptHandler != nullptr,
        "actual production DriverEntry registers native ISR and HandleInterrupt");
    {
        auto& adapter = fresh();
        BOOLEAN ours = TRUE, wrapper = TRUE;
        callbacks.Ndis50Chars.Ndis40Chars.Ndis30Chars.ISRHandler(&ours, &wrapper, nullptr);
        expect(!ours && !wrapper, "null ISR context clears outputs");
        adapter.resources.interrupt_flags = 2;
        expect(register_interrupt(adapter) == NDIS_STATUS_INVALID_DATA && f.registered == 0,
            "message interrupt resources cannot silently become line interrupts");
        adapter.resources.interrupt_flags = 0;
        adapter.resources.interrupt_vector = 0;
        expect(register_interrupt(adapter) == NDIS_STATUS_INVALID_DATA, "missing line vector rejected");
    }
    {
        auto& adapter = fresh();
        f.fail_register = true;
        expect(register_interrupt(adapter) == NDIS_STATUS_RESOURCE_CONFLICT && !adapter.interrupts.registered &&
            retire_interrupt(adapter), "NDIS registration conflict unwinds without false disconnect");
    }
    {
        auto& adapter = fresh();
        start(adapter);
        expect(f.vector == 11 && f.level == 11 && f.isr && f.shared && f.mode == NdisInterruptLevelSensitive,
            "assigned level and sharing metadata reach genuine NDIS signature");
        fire(adapter, 0, false);
        fire(adapter, 0x80000000, false);
        expect(f.queued == nullptr && f.mask == interrupt_event_mask, "unrelated shared-line events cause no mask or DPC");
        f.order_count = 0;
        fire(adapter, 1, true);
        expect(f.order_count == 5 && memcmp(f.order, "RMFAF", 5) == 0,
            "recognized line is mask-flushed before W1C ACK-flush");
        expect(f.mask == 0 && adapter.interrupts.references == 1, "accepted ISR owns one private queued callback");
        run_dpc();
        expect(f.mask == interrupt_event_mask && f.tx_calls == 1 && f.rx_calls == 1 &&
            adapter.interrupts.references == 0, "bounded service rearms after empty final status");
        fire(adapter, 4, true);
        f.tx_budget = interrupt_packet_budget; f.rx_budget = interrupt_packet_budget;
        run_dpc();
        expect(f.mask == 0 && f.queued != nullptr && adapter.interrupts.references == 1,
            "budget exhaustion keeps mask closed and schedules a later continuation");
        run_dpc();
        expect(f.mask == interrupt_event_mask && adapter.interrupts.references == 0,
            "continuation drains tail and restores events");
        f.inject_enable = true;
        fire(adapter, 4, true); run_dpc();
        expect(f.mask == 0 && f.queued != nullptr, "completion racing unmask is masked and captured again");
        run_dpc();
        expect(f.mask == interrupt_event_mask, "enable-race continuation recovers normal IRQ admission");
        fire(adapter, 0x20, true); run_dpc();
        expect(f.work != nullptr && f.link_calls == 0, "link interrupt queues passive work instead of DIRQL PHY I/O");
        fire(adapter, 0x20, true); run_dpc();
        expect(f.queued == nullptr, "in-flight passive link query cannot trigger a DPC storm");
        run_work(); run_dpc(); run_work(); run_dpc();
        expect(f.link_calls == 2 && adapter.link_up, "coalesced link changes receive a second passive sample");
        fire(adapter, 1, true);
        expect(retire_interrupt(adapter) && f.deregistered == 1 && adapter.interrupts.references == 0,
            "stop removes queued DPC and disconnects only after refs drain");
        fire(adapter, 1, false);
        expect(retire_interrupt(adapter) && f.deregistered == 1, "retirement is idempotent without late MMIO");
    }
    {
        auto& adapter = fresh();
        adapter.resources.interrupt_flags = CM_RESOURCE_INTERRUPT_LATCHED;
        adapter.resources.interrupt_shared = FALSE;
        start(adapter);
        expect(f.mode == NdisInterruptLatched && !f.shared, "assigned exclusive latched route remains explicit");
        fire(adapter, 0xffff, false);
        expect(adapter.interrupts.removed && f.queued == nullptr && !interrupt_registered(adapter),
            "low16 all-ones marks removal and stops access without claiming shared IRQ");
        const auto writes = f.writes;
        expect(retire_interrupt(adapter) && f.writes == writes, "removal teardown never writes inaccessible BAR");
    }
    {
        auto& adapter = fresh(); start(adapter);
        f.fail_flush = true; fire(adapter, 1, true);
        expect(adapter.interrupts.faulted && f.queued == nullptr, "posted-write failure closes service without a storm");
        expect(retire_interrupt(adapter), "failed IRQ I/O still fences callbacks before owner handoff");
    }
    {
        auto& adapter = fresh(); start(adapter);
        fire(adapter, 0x20, true); run_dpc();
        expect(f.work != nullptr && adapter.interrupts.references == 1, "queued passive work holds adapter reference");
        expect(retire_interrupt(adapter) && f.sleeps == 1 && f.link_calls == 0,
            "stop waits queued passive worker and prevents late PHY access");
    }
    {
        auto& adapter = fresh(); start(adapter);
        f.fail_link = true; fire(adapter, 0x20, true); run_dpc(); run_work();
        expect(adapter.hardware.device.recovery_required && !adapter.datapath.running && !adapter.link_up,
            "PHY read failure closes packet path and hands recovery requirement to M08");
        expect(retire_interrupt(adapter), "failed link work releases its tracked reference");
    }
    {
        auto& adapter = fresh(); start(adapter);
        fire(adapter, 0x20, true); run_dpc();
        f.force_wait = true;
        expect(!retire_interrupt(adapter) && f.sleeps == 10000 && f.deregistered == 0 &&
            adapter.interrupts.registered && adapter.interrupts.references == 1,
            "callback timeout retains IRQ adapter mapping and queued owner rather than inventing retirement");
        f.force_wait = false; run_work();
        expect(retire_interrupt(adapter) && f.deregistered == 1,
            "later genuine callback exit permits retried retirement");
    }
    {
        auto& adapter = fresh(); start(adapter);
        f.fail_schedule = true; fire(adapter, 0x20, true); run_dpc();
        expect(adapter.interrupts.closing && adapter.interrupts.references == 0 && f.mask == 0,
            "work scheduling failure releases its reserved ref and leaves hardware masked");
        expect(retire_interrupt(adapter), "failed work publication still permits tracked teardown");
    }
    {
        auto& adapter = fresh(); start(adapter);
        fire(adapter, 1, true);
        adapter.dispatch_gate = 1;
        run_dpc();
        expect(f.tx_calls == 0 && f.rx_calls == 0 && f.queued != nullptr &&
            adapter.interrupts.references == 1,
            "concurrent packet callback rejects DPC admission and retains a bounded continuation");
        adapter.dispatch_gate = 0;
        run_dpc();
        expect(f.tx_calls == 1 && f.rx_calls == 1 && f.mask == interrupt_event_mask && f.irql == PASSIVE_LEVEL,
            "retry drains once after packet serialization releases and restores caller IRQL");
        expect(retire_interrupt(adapter), "serialization retry leaves no callback owner behind");
    }
    {
        auto& adapter = fresh(); start(adapter);
        const auto writes = f.writes;
        callbacks.PnPEventNotifyHandler(&adapter, NdisDevicePnPEventSurpriseRemoved, nullptr, 0);
        expect(adapter.interrupts.removed && adapter.stop_requested && !adapter.datapath.running &&
            f.writes == writes, "actual PnP surprise removal closes IRQ without an inaccessible BAR write");
        expect(retire_interrupt(adapter), "surprise removal drains owners and disconnects assigned line");
    }
    printf("%u genuine-SDK interrupt checks, %u failures\n", f.checks, f.failures);
    return f.failures == 0 ? 0 : 1;
}
