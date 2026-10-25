// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.recovery_ndis
Purpose: Exercise compiled patched NDIS lifetime gate and actual power dispatch.
Created: 2026-10-24
Architecture: Genuine private wrapper headers; kernel and lower PCI IRPs mocked.
*/
extern "C" {
#include "ndissys.h"
}
#include <cstdio>
#include <cstdlib>
#include <new>
namespace {
struct Work { PIO_WORKITEM_ROUTINE routine{}; PVOID context{}; PDEVICE_OBJECT device{}; bool freed{}; };
struct Fixture {
    LOGICAL_ADAPTER adapter{};
    DEVICE_OBJECT device{}, lower{};
    struct IrpStorage { IRP irp{}; IO_STACK_LOCATION stack[2]{}; } storage{};
    Work work{};
    unsigned checks{}, failures{}, calls{}, lower_calls{}, completed{}, started{}, allocations{}, frees{}, references{};
    ULONG order{}, oid_order{}, lower_order{};
    bool fail_pool{}, fail_work{}, fail_oid{}, fail_lower{};
    KIRQL irql{};
} f;
void expect(bool okay, const char* text)
{ ++f.checks; if (!okay) { ++f.failures; std::printf("FAIL: %s\n", text); } }
void fresh()
{
    const auto checks = f.checks, failures = f.failures;
    f.~Fixture(); new (&f) Fixture{}; f.checks = checks; f.failures = failures;
    f.device.DeviceExtension = &f.adapter;
    f.adapter.NdisMiniportBlock.NextDeviceObject = &f.lower;
    f.adapter.NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceStarted;
    auto& irp = f.storage.irp;
    irp.StackCount = 2; irp.CurrentLocation = 2; irp.Tail.Overlay.CurrentStackLocation = &f.storage.stack[1];
    auto& stack = f.storage.stack[1]; stack.MinorFunction = IRP_MN_SET_POWER;
    stack.Parameters.Power.Type = DevicePowerState;
}
KIRQL FASTCALL acquire(PKSPIN_LOCK) { auto old = f.irql; f.irql = DISPATCH_LEVEL; return old; }
VOID FASTCALL release(PKSPIN_LOCK, KIRQL old) { f.irql = old; }
VOID NTAPI clear_event(PRKEVENT event) { event->Header.SignalState = 0; }
LONG NTAPI set_event(PRKEVENT event, KPRIORITY, BOOLEAN) { event->Header.SignalState = 1; return 0; }
PVOID NTAPI allocate(POOL_TYPE, SIZE_T bytes)
{ if (f.fail_pool) { return nullptr; } ++f.allocations; return std::malloc(bytes); }
VOID NTAPI free_pool(PVOID pointer) { ++f.frees; std::free(pointer); }
PIO_WORKITEM NTAPI allocate_work(PDEVICE_OBJECT device)
{ if (f.fail_work) { return nullptr; } f.work.device = device; return reinterpret_cast<PIO_WORKITEM>(&f.work); }
VOID NTAPI free_work(PIO_WORKITEM work) { reinterpret_cast<Work*>(work)->freed = true; }
VOID NTAPI queue_work(PIO_WORKITEM work, PIO_WORKITEM_ROUTINE routine, WORK_QUEUE_TYPE, PVOID context)
{ auto& item = *reinterpret_cast<Work*>(work); item.routine = routine; item.context = context; }
LONG_PTR FASTCALL reference(PVOID) { return ++f.references; }
LONG_PTR FASTCALL dereference(PVOID) { return --f.references; }
VOID FASTCALL complete(PIRP irp, CCHAR)
{ expect(irp == &f.storage.irp, "completion owns the original IRP"); ++f.completed; }
VOID NTAPI start_next(PIRP) { ++f.started; }
BOOLEAN NTAPI cancel_timer(PKTIMER) { return TRUE; }
BOOLEAN NTAPI remove_dpc(PKDPC) { return TRUE; }
KAFFINITY NTAPI active_processors() { return 0; }
VOID NTAPI affinity(KAFFINITY) {}
VOID NTAPI revert_affinity() {}
NTSTATUS NTAPI wait_object(PVOID, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER)
{ return STATUS_TIMEOUT; }
NTSTATUS NTAPI call_lower(PDEVICE_OBJECT device, PIRP irp)
{
    expect(device == &f.lower, "power forwards to the actual attached lower device");
    ++f.lower_calls; f.lower_order = ++f.order;
    irp->IoStatus.Status = f.fail_lower ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
    auto& next = f.storage.stack[0];
    if (next.CompletionRoutine != nullptr) { next.CompletionRoutine(&f.device, irp, next.Context); }
    return irp->IoStatus.Status;
}
void run_work()
{
    auto routine = f.work.routine; f.work.routine = nullptr;
    expect(routine != nullptr, "a real I/O worker was published");
    if (routine != nullptr) { routine(f.work.device, f.work.context); }
}
}
extern "C" NDIS_STATUS MiniSetInformation(PLOGICAL_ADAPTER adapter, NDIS_OID oid,
                                           ULONG size, PVOID state, PULONG read)
{
    expect(adapter == &f.adapter && oid == OID_PNP_SET_POWER && size == sizeof(NDIS_DEVICE_POWER_STATE) &&
        state != nullptr && adapter->LifecycleReferences == 1,
        "power OID spans a genuine wrapper callback reference");
    ++f.calls; f.oid_order = ++f.order; *read = size;
    return f.fail_oid ? NDIS_STATUS_FAILURE : NDIS_STATUS_SUCCESS;
}
#define MOCK(name, bytes, function) decltype(&name) imp_##name __asm__("__imp__" #name "@" #bytes) = function
extern "C" {
MOCK(KeClearEvent, 4, clear_event);
MOCK(KeSetEvent, 12, set_event);
MOCK(ExAllocatePool, 8, allocate);
MOCK(ExFreePool, 4, free_pool);
MOCK(IoAllocateWorkItem, 4, allocate_work);
MOCK(IoFreeWorkItem, 4, free_work);
MOCK(IoQueueWorkItem, 16, queue_work);
MOCK(PoStartNextPowerIrp, 4, start_next);
MOCK(PoCallDriver, 8, call_lower);
MOCK(KeCancelTimer, 4, cancel_timer);
MOCK(KeRemoveQueueDpc, 4, remove_dpc);
MOCK(KeQueryActiveProcessors, 0, active_processors);
MOCK(KeSetSystemAffinityThread, 4, affinity);
MOCK(KeRevertToUserAffinityThread, 0, revert_affinity);
MOCK(KeWaitForSingleObject, 20, wait_object);
decltype(&KfAcquireSpinLock) imp_lock __asm__("__imp_@KfAcquireSpinLock@4") = acquire;
decltype(&KfReleaseSpinLock) imp_unlock __asm__("__imp_@KfReleaseSpinLock@8") = release;
decltype(&ObfReferenceObject) imp_reference __asm__("__imp_@ObfReferenceObject@4") = reference;
decltype(&ObfDereferenceObject) imp_dereference __asm__("__imp_@ObfDereferenceObject@4") = dereference;
decltype(&IofCompleteRequest) imp_complete __asm__("__imp_@IofCompleteRequest@8") = complete;
}
int main()
{
    fresh();
    expect(MiniAcquireLifecycleReference(&f.adapter) && f.adapter.LifecycleReferences == 1 &&
        !f.adapter.LifecycleDrained.Header.SignalState, "callback reservation precedes entering driver code");
    f.adapter.LifecycleClosing = TRUE;
    expect(!MiniAcquireLifecycleReference(&f.adapter) && f.adapter.LifecycleReferences == 1,
        "queued and preempted future entries reject close without consuming old reference");
    MiniReleaseLifecycleReference(&f.adapter);
    expect(f.adapter.LifecycleReferences == 0 && f.adapter.LifecycleDrained.Header.SignalState,
        "last callback release follows actual driver return and signals passive drain");
    for (unsigned close = 0; close < 2; ++close) {
        fresh(); f.storage.stack[1].Parameters.Power.State.DeviceState = PowerDeviceD3;
        expect(NdisIPower(&f.device, &f.storage.irp) == STATUS_PENDING && f.calls == 0 && f.lower_calls == 0,
            "Dx callback queues bounded passive work without driver polling at dispatch");
        f.adapter.LifecycleClosing = close != 0; run_work();
        if (close) {
            expect(f.calls == 0 && f.lower_calls == 0 && f.completed == 1 &&
                f.storage.irp.IoStatus.Status == STATUS_DELETE_PENDING, "late closed worker never enters miniport or PCI power");
        } else {
            expect(f.calls == 1 && f.lower_calls == 1 && f.oid_order < f.lower_order,
                "Dx positively quiesces NIC before lower PCI power transition");
        }
        expect(f.adapter.LifecycleReferences == 0 && f.references == 0 && f.frees == 1 && f.work.freed && f.started == 1,
            "power work and FDO ownership unwind exactly once");
    }
    fresh(); f.storage.stack[1].Parameters.Power.State.DeviceState = PowerDeviceD0;
    expect(NdisIPower(&f.device, &f.storage.irp) == STATUS_PENDING && f.lower_calls == 1 && f.calls == 0,
        "D0 first restores actual lower PCI device before asynchronous restart");
    run_work();
    expect(f.calls == 1 && f.oid_order > f.lower_order && f.completed == 1 && f.started == 1 && f.references == 0,
        "D0 restarts MAC/PHY/rings after lower completion and completes original IRP once");
    for (unsigned failure = 0; failure < 4; ++failure) {
        fresh(); f.storage.stack[1].Parameters.Power.State.DeviceState = failure == 3 ? PowerDeviceD0 : PowerDeviceD3;
        f.fail_pool = failure == 0; f.fail_work = failure == 1; f.fail_oid = failure == 2; f.fail_lower = failure == 3;
        NdisIPower(&f.device, &f.storage.irp);
        if (f.work.routine != nullptr) { run_work(); }
        expect(f.completed == 1 && !NT_SUCCESS(f.storage.irp.IoStatus.Status) && f.references == 0 &&
            f.allocations == f.frees && f.started == 1, "allocation, OID and lower power failures complete once without leaked ownership");
        expect(failure != 2 || f.lower_calls == 0, "failed NIC retirement prevents PCI powerdown");
        expect(failure != 3 || f.calls == 0, "failed lower D0 transition prevents inaccessible PHY/MMIO access");
    }
    fresh(); f.adapter.NdisMiniportBlock.PnPDeviceState = NdisPnPDeviceStopped;
    f.storage.stack[1].Parameters.Power.State.DeviceState = PowerDeviceD0;
    NdisIPower(&f.device, &f.storage.irp);
    expect(f.lower_calls == 1 && f.calls == 0 && f.work.routine == nullptr,
        "initial D0 before Initialize bypasses unregistered miniport power callbacks");
    std::printf("%u compiled patched-NDIS checks, %u failures\n", f.checks, f.failures);
    return f.failures == 0 ? 0 : 1;
}
