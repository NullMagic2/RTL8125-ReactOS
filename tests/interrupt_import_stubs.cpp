// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.interrupt_import_stubs
Purpose: Link real M07 code into the historical probe and packet ABI fixtures.
Created: 2026-10-24
Architecture: Genuine NDIS imports are mocked; dedicated IRQ suite models events.
*/
#include "../src/reactos/adapter.hpp"
namespace {
PKDPC queued{};
PNDIS_WORK_ITEM work{};
NDIS_WORK_ITEM link_work{};
VOID NTAPI link(PNDIS_WORK_ITEM, PVOID context)
{ rtl8125::reactos::service_interrupt_link(*static_cast<rtl8125::reactos::Adapter*>(context)); }
bool dispatch_link(rtl8125::reactos::Adapter& adapter) noexcept
{
    if (work != nullptr) { return false; }
    NdisInitializeWorkItem(&link_work, link, &adapter); work = &link_work; return true;
}
KIRQL FASTCALL raise(KIRQL) { return KeGetCurrentIrql(); }
VOID FASTCALL lower(KIRQL) {}
VOID NTAPI initialize(PKDPC dpc, PKDEFERRED_ROUTINE routine, PVOID context)
{ RtlZeroMemory(dpc, sizeof(*dpc)); dpc->DeferredRoutine = routine; dpc->DeferredContext = context; }
BOOLEAN NTAPI insert(PKDPC dpc, PVOID, PVOID)
{ if (queued != nullptr) { return FALSE; } queued = dpc; return TRUE; }
BOOLEAN NTAPI remove(PKDPC dpc)
{ if (queued != dpc) { return FALSE; } queued = nullptr; return TRUE; }
NDIS_STATUS NTAPI connect(PNDIS_MINIPORT_INTERRUPT interrupt, NDIS_HANDLE, UINT, UINT, BOOLEAN, BOOLEAN, NDIS_INTERRUPT_MODE)
{
    auto* owner = reinterpret_cast<rtl8125::reactos::InterruptOwner*>(interrupt);
    owner->schedule_link = dispatch_link;
    return NDIS_STATUS_SUCCESS;
}
VOID NTAPI disconnect(PNDIS_MINIPORT_INTERRUPT) {}
BOOLEAN NTAPI synchronize(PNDIS_MINIPORT_INTERRUPT, PVOID function, PVOID context)
{ return reinterpret_cast<PKSYNCHRONIZE_ROUTINE>(function)(context); }
NDIS_STATUS NTAPI schedule(PNDIS_WORK_ITEM item)
{ if (work != nullptr) { return NDIS_STATUS_FAILURE; } work = item; return NDIS_STATUS_SUCCESS; }
}
void interrupt_fixture_drain_work() noexcept
{
    auto* item = work; work = nullptr;
    if (item != nullptr) { item->Routine(item, item->Context); }
}
void interrupt_fixture_drain_dpc() noexcept
{
    auto* dpc = queued; queued = nullptr;
    if (dpc != nullptr) { dpc->DeferredRoutine(dpc, dpc->DeferredContext, nullptr, nullptr); }
}
namespace rtl8125::reactos {
NDIS_STATUS query_hardware_link(HardwareAccess&, const HardwareState&, hardware::LinkState& output) noexcept
{ output = {}; return NDIS_STATUS_SUCCESS; }
}
#define MOCK(name, bytes, implementation) \
    decltype(&name) import_##name __asm__("__imp__" #name "@" #bytes) = implementation
extern "C" {
MOCK(KeInitializeDpc, 12, initialize);
MOCK(KeInsertQueueDpc, 12, insert);
MOCK(KeRemoveQueueDpc, 4, remove);
MOCK(NdisMRegisterInterrupt, 28, connect);
MOCK(NdisMDeregisterInterrupt, 4, disconnect);
MOCK(NdisMSynchronizeWithInterrupt, 12, synchronize);
MOCK(NdisScheduleWorkItem, 4, schedule);
decltype(&KfRaiseIrql) import_raise __asm__("__imp_@KfRaiseIrql@4") = raise;
decltype(&KfLowerIrql) import_lower __asm__("__imp_@KfLowerIrql@4") = lower;
}
