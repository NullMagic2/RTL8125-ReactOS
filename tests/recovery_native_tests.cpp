// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.recovery_native
Purpose: Exercise actual M08 on a concurrently executing joinable Win32 thread.
Created: 2026-10-24
Architecture: Genuine NT/NDIS ABI; OS, packet and hardware boundaries are mocked.
*/
#include "../src/reactos/adapter.hpp"
#include <cstdio>
#include <new>
extern "C" {
HANDLE __stdcall host_create_thread(PVOID, SIZE_T, ULONG (__stdcall*)(PVOID), PVOID, ULONG, PULONG) __asm__("_CreateThread@24");
HANDLE __stdcall host_create_event(PVOID, int, int, const char*) __asm__("_CreateEventA@16");
int __stdcall host_set_event(HANDLE) __asm__("_SetEvent@4");
ULONG __stdcall host_wait(HANDLE, ULONG) __asm__("_WaitForSingleObject@8");
int __stdcall host_close(HANDLE) __asm__("_CloseHandle@4");
VOID __stdcall host_sleep(ULONG) __asm__("_Sleep@4");
VOID __stdcall host_exit_thread(ULONG) __asm__("_ExitThread@4");
}
using namespace rtl8125;
using namespace rtl8125::reactos;
namespace {
struct Fixture {
    Adapter adapter{};
    NDIS_MINIPORT_BLOCK block{};
    DRIVER_OBJECT driver{};
    DEVICE_OBJECT functional{};
    DriverRecoveryRegistry registry{};
    HANDLE event{};
    PKSTART_ROUTINE routine{};
    PVOID thread_context{};
    volatile LONG reset_completions{}, power_completions{}, references{};
    volatile LONG device_references{};
    unsigned checks{}, failures{}, starts{}, stops{}, links{}, firmware_frees{}, power_writes{}, joins{}, fences{}, disposals{};
    bool fail_init{}, fail_stop{}, fail_thread{}, fail_firmware{}, fail_join{}, fail_irq{}, fail_enable{}, fail_busmaster{};
    bool actual_cpu_busy{}, affinity_fail{}, unexpected_free{}, memory_live{};
    NDIS_STATUS reset_status{}, power_status{};
    ULONGLONG clock{1};
    ULONG processor{};
    std::uint32_t phy_command{};
} f;
thread_local KIRQL irql = PASSIVE_LEVEL;
void expect(bool okay, const char* text)
{
    ++f.checks;
    if (!okay) { ++f.failures; std::printf("FAIL: %s\n", text); }
}
bool wait_count(volatile LONG& value, LONG count)
{
    for (unsigned n = 0; n < 2000; ++n) {
        if (InterlockedCompareExchange(&value, 0, 0) == count) { return true; }
        host_sleep(1);
    }
    return false;
}
ULONG __stdcall thread_entry(PVOID)
{
    f.routine(f.thread_context);
    return 0;
}
KIRQL NTAPI current_irql() { return irql; }
KIRQL FASTCALL raise_irql(KIRQL value) { auto previous = irql; irql = value; return previous; }
VOID FASTCALL lower_irql(KIRQL value) { irql = value; }
KIRQL FASTCALL acquire_spin(PKSPIN_LOCK lock)
{
    auto previous = irql; irql = DISPATCH_LEVEL;
    while (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(lock), 1, 0) != 0) { host_sleep(0); }
    return previous;
}
VOID FASTCALL release_spin(PKSPIN_LOCK lock, KIRQL previous)
{ InterlockedExchange(reinterpret_cast<volatile LONG*>(lock), 0); irql = previous; }
VOID NTAPI initialize_event(PRKEVENT, EVENT_TYPE, BOOLEAN) { f.event = host_create_event(nullptr, FALSE, FALSE, nullptr); }
LONG NTAPI set_event(PRKEVENT, KPRIORITY, BOOLEAN) { host_set_event(f.event); return 0; }
NTSTATUS NTAPI wait_event(PVOID object, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER timeout)
{
    if (object != &f.adapter.recovery.wake) {
        ++f.joins;
        if (f.fail_join) { return STATUS_TIMEOUT; }
        return host_wait(object, 2000) == 0 ? STATUS_SUCCESS : STATUS_TIMEOUT;
    }
    const auto result = host_wait(f.event, timeout == nullptr ? 2000 : static_cast<ULONG>(-timeout->QuadPart / 10000));
    return result == 0 ? STATUS_SUCCESS : STATUS_TIMEOUT;
}
NTSTATUS NTAPI create_thread(PHANDLE output, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, PCLIENT_ID,
                            PKSTART_ROUTINE routine, PVOID context)
{
    if (f.fail_thread) { return STATUS_INSUFFICIENT_RESOURCES; }
    f.routine = routine; f.thread_context = context;
    *output = host_create_thread(nullptr, 0, thread_entry, nullptr, 0, nullptr);
    return *output != nullptr ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
NTSTATUS NTAPI terminate_thread(NTSTATUS status) { host_exit_thread(static_cast<ULONG>(status)); return status; }
NTSTATUS NTAPI reference_thread(HANDLE thread, ACCESS_MASK, POBJECT_TYPE, KPROCESSOR_MODE, PVOID* output, POBJECT_HANDLE_INFORMATION)
{ *output = thread; return STATUS_SUCCESS; }
NTSTATUS NTAPI close_handle(HANDLE handle) { host_close(handle); return STATUS_SUCCESS; }
VOID NTAPI sleep_ndis(ULONG microseconds) { host_sleep(microseconds > 1000 ? 1 : 0); }
ULONGLONG NTAPI clock_now() { return f.clock; }
KAFFINITY NTAPI active_processors() { return 3; }
VOID NTAPI set_affinity(KAFFINITY mask) { f.processor = f.affinity_fail ? 2 : mask == 1 ? 0 : 1; ++f.fences; }
VOID NTAPI revert_affinity() { f.processor = 0; }
ULONG processor_number() { return f.processor; }
LONG_PTR FASTCALL reference(PVOID object)
{ return object == &f.driver ? InterlockedIncrement(&f.references) : InterlockedIncrement(&f.device_references); }
LONG_PTR FASTCALL dereference(PVOID object)
{ return object == &f.driver ? InterlockedDecrement(&f.references) : object == &f.functional ? InterlockedDecrement(&f.device_references) : 0; }
VOID NTAPI device_property(NDIS_HANDLE, PDEVICE_OBJECT*, PDEVICE_OBJECT* functional, PDEVICE_OBJECT*,
                          PCM_RESOURCE_LIST*, PCM_RESOURCE_LIST*)
{ if (functional != nullptr) { *functional = &f.functional; } }
VOID NTAPI reset_complete(NDIS_HANDLE, NDIS_STATUS status, BOOLEAN addressing)
{
    expect(!addressing, "restored addresses never ask broken wrapper to restore filters");
    f.reset_status = status;
    InterlockedIncrement(&f.reset_completions);
    f.block.ResetStatus = status;
}
VOID NTAPI power_complete(NDIS_HANDLE, NDIS_STATUS status)
{ f.power_status = status; InterlockedIncrement(&f.power_completions); }
void dispose(Adapter*) noexcept { ++f.disposals; }
void fresh()
{
    const auto checks = f.checks, failures = f.failures;
    f.~Fixture(); new (&f) Fixture{};
    f.checks = checks; f.failures = failures;
    f.registry.driver = &f.driver; f.registry.release_adapter = dispose;
    f.functional.DriverObject = &f.driver;
    f.block.ResetCompleteHandler = reset_complete; f.block.SetCompleteHandler = power_complete;
    f.adapter.handle = &f.block;
    f.adapter.resources.mapped_address = reinterpret_cast<PUCHAR>(0x1000);
    f.adapter.resources.chip = {ChipStatus::supported, MacVersion::rtl8125b, 0x641};
    f.adapter.hardware_access.adapter_handle = &f.block;
    f.adapter.phase = AdapterPhase::incomplete;
}
void cleanup()
{
    f.fail_stop = false; f.fail_join = false; f.affinity_fail = false;
    if (f.adapter.recovery.thread != nullptr || f.adapter.recovery.registry != nullptr) {
        expect(halt_recovery(f.adapter), "owned thread and resources retire after failure removed");
    }
    if (f.event != nullptr) { host_close(f.event); f.event = nullptr; }
}
bool claim(void*) noexcept { return true; }
void release(void*) noexcept {}
bool hw_read(void*, std::uint32_t offset, unsigned, std::uint32_t& value) noexcept
{ value = offset == 0xb8 && (f.phy_command & 0x80000000) == 0 ? 0x80000000 : 0; return true; }
bool hw_write(void*, std::uint32_t offset, unsigned, std::uint32_t value) noexcept
{ if (offset == 0xb8) { f.phy_command = value; } ++f.power_writes; return true; }
bool hw_delay(void*, std::uint32_t) noexcept { return true; }
std::uint64_t hw_now(void*) noexcept { return 0; }
}
namespace rtl8125::reactos {
bool rendezvous_adapter_processors() noexcept { f.fences += 2; return !f.affinity_fail; }
NDIS_STATUS acquire_firmware(MacVersion version, FirmwareBlob& blob) noexcept
{
    if (f.fail_firmware) { return NDIS_STATUS_FILE_NOT_FOUND; }
    blob.bytes = reinterpret_cast<PUCHAR>(0x2000); blob.size = 4; blob.version = version; return NDIS_STATUS_SUCCESS;
}
void release_firmware(FirmwareBlob& blob) noexcept
{ if (blob.bytes != nullptr) { ++f.firmware_frees; } blob = {}; }
hardware::RegisterIo hardware_register_io(HardwareAccess&) noexcept
{ return {&f, claim, release, hw_read, hw_write, hw_delay, hw_now}; }
NDIS_STATUS initialize_hardware(HardwareAccess&, const FirmwareBlob&, HardwareState& state) noexcept
{
    ++f.starts;
    if (f.fail_init) { return NDIS_STATUS_FAILURE; }
    state.device.initialized = true; state.device.version = MacVersion::rtl8125b; return NDIS_STATUS_SUCCESS;
}
NDIS_STATUS activate_hardware(HardwareAccess&, HardwareState& state, const hardware::RingConfiguration&) noexcept
{ state.device.running = true; return NDIS_STATUS_SUCCESS; }
NDIS_STATUS register_interrupt(Adapter& adapter) noexcept
{ if (f.fail_irq) { return NDIS_STATUS_FAILURE; } adapter.interrupts.registered = true; adapter.interrupts.adapter = &adapter; return NDIS_STATUS_SUCCESS; }
NDIS_STATUS enable_interrupt(Adapter& adapter) noexcept
{ if (f.fail_enable) { return NDIS_STATUS_FAILURE; } return adapter.interrupts.schedule_link != nullptr ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE; }
void close_interrupt_admission(Adapter& adapter, bool removed) noexcept
{ adapter.interrupts.closing = 1; adapter.interrupts.removed = removed; }
bool retire_interrupt(Adapter& adapter) noexcept
{ adapter.interrupts.registered = false; adapter.interrupts.adapter = nullptr; return true; }
void service_interrupt_link(Adapter&) noexcept { ++f.links; }
void defer_interrupt_service(Adapter&) noexcept {}
bool interrupt_registered(const Adapter& adapter) noexcept { return adapter.interrupts.registered; }
NDIS_STATUS prepare_packet_path(Adapter& adapter, const PacketPrerequisites& prerequisites) noexcept
{ adapter.packet_prerequisites = prerequisites; adapter.datapath.adapter = adapter.handle; adapter.phase = AdapterPhase::prepared; f.memory_live = true; return NDIS_STATUS_SUCCESS; }
NDIS_STATUS activate_packet_path(Adapter& adapter) noexcept
{
    if (!adapter.packet_prerequisites.activate(&adapter, adapter.dma)) { return NDIS_STATUS_FAILURE; }
    adapter.phase = AdapterPhase::running; adapter.datapath.running = true; return NDIS_STATUS_SUCCESS;
}
NDIS_STATUS stop_packet_path(Adapter& adapter) noexcept
{
    ++f.stops;
    if (f.fail_stop) { return NDIS_STATUS_FAILURE; }
    if (adapter.packet_prerequisites.retire_cpu_access != nullptr &&
        !adapter.packet_prerequisites.retire_cpu_access(&adapter)) { return NDIS_STATUS_FAILURE; }
    if (adapter.recovery.phase != RecoveryPhase::dormant && !adapter.recovery.cpu_fenced) { f.unexpected_free = true; }
    f.memory_live = false; adapter.phase = AdapterPhase::incomplete; adapter.datapath.adapter = nullptr;
    adapter.hardware = {}; adapter.packet_prerequisites = {}; return NDIS_STATUS_SUCCESS;
}
void stop_datapath(Datapath& path) noexcept { path.running = false; }
DmaStatus expose_dma_rings(DmaOwner&) noexcept { return DmaStatus::ok; }
DmaStatus enable_dma_bus_master(DmaOwner&, const DeviceResources&) noexcept
{ return f.fail_busmaster ? DmaStatus::retirement_failed : DmaStatus::ok; }
bool write_mmio32(const DeviceResources&, ULONG, ULONG) noexcept { return true; }
bool read_mmio8(const DeviceResources&, ULONG, UCHAR& value) noexcept { value = 0x0c; return true; }
bool write_mmio8(const DeviceResources&, ULONG, UCHAR) noexcept { return true; }
bool flush_posted_writes(const DeviceResources&) noexcept { return true; }
}
#define MOCK(name, bytes, implementation) decltype(&name) import_##name __asm__("__imp__" #name "@" #bytes) = implementation
extern "C" {
MOCK(KeGetCurrentIrql, 0, current_irql);
MOCK(NdisMGetDeviceProperty, 24, device_property);
MOCK(KeInitializeEvent, 12, initialize_event);
MOCK(KeSetEvent, 12, set_event);
MOCK(KeWaitForSingleObject, 20, wait_event);
MOCK(PsCreateSystemThread, 28, create_thread);
MOCK(PsTerminateSystemThread, 4, terminate_thread);
MOCK(ObReferenceObjectByHandle, 24, reference_thread);
MOCK(ZwClose, 4, close_handle);
MOCK(NdisMSleep, 4, sleep_ndis);
MOCK(KeQueryInterruptTime, 0, clock_now);
MOCK(KeQueryActiveProcessors, 0, active_processors);
MOCK(KeSetSystemAffinityThread, 4, set_affinity);
MOCK(KeRevertToUserAffinityThread, 0, revert_affinity);
decltype(&KeGetCurrentProcessorNumber) imp_processor __asm__("__imp__KeGetCurrentProcessorNumber") = processor_number;
decltype(&KfRaiseIrql) imp_raise __asm__("__imp_@KfRaiseIrql@4") = raise_irql;
decltype(&KfLowerIrql) imp_lower __asm__("__imp_@KfLowerIrql@4") = lower_irql;
decltype(&KfAcquireSpinLock) imp_acquire __asm__("__imp_@KfAcquireSpinLock@4") = acquire_spin;
decltype(&KfReleaseSpinLock) imp_release __asm__("__imp_@KfReleaseSpinLock@8") = release_spin;
decltype(&ObfReferenceObject) imp_reference __asm__("__imp_@ObfReferenceObject@4") = reference;
decltype(&ObfDereferenceObject) imp_dereference __asm__("__imp_@ObfDereferenceObject@4") = dereference;
POBJECT_TYPE thread_type{};
POBJECT_TYPE* import_thread_type __asm__("__imp__PsThreadType") = &thread_type;
}
int main()
{
    fresh();
    BOOLEAN addressing = TRUE;
    expect(request_reset(f.adapter, &addressing) == NDIS_STATUS_ADAPTER_NOT_READY && !addressing,
        "read-only probe owns no recovery thread and rejects reset truthfully");
    expect(!check_for_hang(f.adapter), "inactive probe never invents a hang");
    expect(start_recovery(f.adapter, f.registry) == NDIS_STATUS_SUCCESS && f.adapter.phase == AdapterPhase::running &&
        f.references == 1 && f.registry.first == &f.adapter, "startup owns actual thread, registry reference and running state");
    expect(f.adapter.interrupts.schedule_link == request_link_service, "IRQ link dispatch uses joined lifecycle thread");
    f.adapter.link_up = true; f.adapter.datapath.outstanding = 1;
    f.clock = 50000001;
    expect(check_for_hang(f.adapter), "five-second TX stall with live link reports actual missing progress");
    f.adapter.datapath.counters.tx_completed = 1;
    expect(!check_for_hang(f.adapter), "genuine TX completion clears stale progress timer");
    f.adapter.datapath.faulted = true;
    expect(check_for_hang(f.adapter), "hardware/datapath failure reports recovery independently of ring progress");
    f.adapter.datapath.faulted = false;
    f.adapter.datapath.filter.flags = filter_broadcast | filter_directed;
    f.adapter.datapath.filter.multicast_count = 1; f.adapter.datapath.filter.multicast[0][0] = 1;
    f.adapter.lookahead = 73;
    expect(request_reset(f.adapter, &addressing) == NDIS_STATUS_PENDING && !addressing, "reset callback is nonblocking and singly owned");
    expect(request_reset(f.adapter, &addressing) == NDIS_STATUS_RESET_IN_PROGRESS, "duplicate reset cannot acquire another generation");
    host_sleep(20);
    expect(f.reset_completions == 0 && f.adapter.recovery.reset_pending, "worker never completes before actual wrapper pending publication");
    f.block.ResetStatus = NDIS_STATUS_PENDING;
    expect(wait_count(f.reset_completions, 1) && f.reset_status == NDIS_STATUS_SUCCESS, "worker completes published reset once after restart");
    expect(f.adapter.datapath.filter.flags == (filter_broadcast | filter_directed) &&
        f.adapter.datapath.filter.multicast_count == 1 && f.adapter.lookahead == 73,
        "restart restores filter, multicast and lookahead before FALSE addressing-reset completion");
    expect(f.adapter.recovery.generation == 1 && f.adapter.recovery.completed_generation == 1,
        "completion belongs to exact accepted reset generation");
    for (unsigned n = 0; n < 12; ++n) {
        f.block.ResetStatus = NDIS_STATUS_PENDING;
        expect(request_reset(f.adapter, &addressing) == NDIS_STATUS_PENDING, "later reset owns a fresh generation");
        expect(wait_count(f.reset_completions, static_cast<LONG>(n + 2)), "each repeated reset completes exactly once");
    }
    expect(!f.unexpected_free && f.fences >= 4, "memory release follows complete CPU fence including both active processors");
    expect(request_power(f.adapter, NdisDeviceStateD3) == NDIS_STATUS_PENDING, "D3 power request is owned asynchronously");
    expect(wait_count(f.power_completions, 1) && f.power_status == NDIS_STATUS_SUCCESS &&
        f.adapter.recovery.current_power == NdisDeviceStateD3 && !f.memory_live, "D3 fences CPU and DMA before PHY powerdown");
    if (f.power_status != NDIS_STATUS_SUCCESS) {
        std::printf("power diagnostics status=%08lx phase=%u writes=%u command=%08x\n", f.power_status,
            static_cast<unsigned>(f.adapter.recovery.phase), f.power_writes, static_cast<unsigned>(f.phy_command));
    }
    const auto writes = f.power_writes;
    expect(request_power(f.adapter, NdisDeviceStateD2) == NDIS_STATUS_PENDING && wait_count(f.power_completions, 2) &&
        f.power_writes == writes, "non-D0 transitions never access a BAR after OS power removal");
    expect(request_power(f.adapter, NdisDeviceStateD0) == NDIS_STATUS_PENDING && wait_count(f.power_completions, 3) &&
        f.adapter.phase == AdapterPhase::running, "D0 restores PHY, rings, filters and actual IRQ ownership");
    expect(request_power(f.adapter, NdisDeviceStateD0) == NDIS_STATUS_PENDING && wait_count(f.power_completions, 4) &&
        !f.adapter.stop_requested && f.adapter.phase == AdapterPhase::running,
        "repeated D0 publication reopens genuine IRQ and packet admission");
    expect(halt_recovery(f.adapter) && f.joins == 1 && f.references == 0 && f.registry.first == nullptr &&
        f.firmware_frees == 1 && !f.memory_live, "Halt joins thread before releasing firmware, memory and image reference");
    cleanup();
    for (unsigned failure = 0; failure < 5; ++failure) {
        fresh();
        f.fail_firmware = failure == 0; f.fail_thread = failure == 1; f.fail_init = failure == 2;
        f.fail_irq = failure == 3; f.fail_busmaster = failure == 4;
        expect(start_recovery(f.adapter, f.registry) != NDIS_STATUS_SUCCESS && f.references == 0 &&
            f.adapter.recovery.thread == nullptr && !f.memory_live, "every partial-start failure unwinds only after actual join/fence");
        cleanup();
    }
    fresh(); expect(start_recovery(f.adapter, f.registry) == NDIS_STATUS_SUCCESS, "retention scenario starts");
    f.fail_stop = true;
    expect(!halt_recovery(f.adapter) && f.memory_live && f.references == 1 && f.registry.first == &f.adapter,
        "uncertain DMA retirement retains adapter, firmware, mapping and driver image");
    f.fail_stop = false;
    expect(retire_driver_recovery(f.registry) && f.references == 0 && f.disposals == 1,
        "driver unload retries retained owners and releases only after real retirement");
    cleanup();
    fresh(); expect(start_recovery(f.adapter, f.registry) == NDIS_STATUS_SUCCESS, "join-timeout scenario starts");
    f.fail_join = true;
    expect(!halt_recovery(f.adapter) && f.memory_live && f.references == 1 && f.firmware_frees == 0,
        "thread timeout cannot justify releasing live callback code or storage");
    cleanup();
    fresh(); expect(start_recovery(f.adapter, f.registry) == NDIS_STATUS_SUCCESS, "surprise-removal scenario starts");
    request_stop(f.adapter, true);
    expect(request_reset(f.adapter, &addressing) == NDIS_STATUS_ADAPTER_NOT_READY,
        "surprise removal permanently rejects reset restart");
    cleanup();
    fresh(); expect(start_recovery(f.adapter, f.registry) == NDIS_STATUS_SUCCESS, "failed-reset recovery scenario starts");
    f.adapter.datapath.filter.flags = filter_multicast | filter_broadcast;
    f.adapter.datapath.filter.multicast_count = 1;
    f.adapter.datapath.filter.multicast[0][0] = 1;
    f.fail_init = true; f.block.ResetStatus = NDIS_STATUS_PENDING;
    expect(request_reset(f.adapter, &addressing) == NDIS_STATUS_PENDING && wait_count(f.reset_completions, 1) &&
        f.reset_status == NDIS_STATUS_FAILURE && f.adapter.stop_requested && !f.memory_live,
        "failed restart reports failure once and keeps admission closed");
    f.fail_init = false; f.block.ResetStatus = NDIS_STATUS_PENDING;
    expect(request_reset(f.adapter, &addressing) == NDIS_STATUS_PENDING && wait_count(f.reset_completions, 2) &&
        f.reset_status == NDIS_STATUS_SUCCESS && f.adapter.datapath.filter.flags == (filter_multicast | filter_broadcast) &&
        f.adapter.datapath.filter.multicast_count == 1,
        "later reset restores desired addressing retained across the failed attempt");
    expect(f.device_references == 1, "live recovery preserves actual NDIS FDO and logical DMA owner");
    cleanup();
    expect(f.device_references == 0, "actual FDO reference releases after complete CPU/DMA/thread retirement");
    fresh(); expect(start_recovery(f.adapter, f.registry) == NDIS_STATUS_SUCCESS, "CPU fence failure scenario starts");
    f.affinity_fail = true;
    expect(!halt_recovery(f.adapter) && f.memory_live && f.references == 1 && f.device_references == 1,
        "unproved DPC code retirement retains both adapter and actual wrapper device");
    cleanup();
    fresh();
    expect(request_reset(f.adapter, nullptr) == NDIS_STATUS_INVALID_DATA, "null reset addressing output is rejected without work");
    expect(request_power(f.adapter, static_cast<NDIS_DEVICE_POWER_STATE>(0)) == NDIS_STATUS_INVALID_DATA &&
        request_power(f.adapter, static_cast<NDIS_DEVICE_POWER_STATE>(5)) == NDIS_STATUS_INVALID_DATA,
        "power values outside exact D0-D3 contract are rejected");
    irql = DISPATCH_LEVEL;
    expect(start_recovery(f.adapter, f.registry) == NDIS_STATUS_ADAPTER_NOT_READY && !halt_recovery(f.adapter),
        "startup and joining never block at dispatch level");
    irql = PASSIVE_LEVEL;
    std::printf("%u genuine-SDK recovery checks, %u failures\n", f.checks, f.failures);
    return f.failures == 0 ? 0 : 1;
}
