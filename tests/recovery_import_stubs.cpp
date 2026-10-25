// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.recovery_import_stubs
Purpose: Isolate M08 in historical M01/M05/M07 ABI fixtures.
Created: 2026-10-24
Architecture: These shims do not exercise or certify recovery implementation.
*/
#include "../src/reactos/adapter.hpp"
namespace {
VOID NTAPI deregister_shutdown(NDIS_HANDLE) {}
}
extern "C" {
decltype(&NdisMDeregisterAdapterShutdownHandler) shutdown_import
    __asm__("__imp__NdisMDeregisterAdapterShutdownHandler@4") = deregister_shutdown;
}
namespace rtl8125::reactos {
NDIS_STATUS request_reset(Adapter&, PBOOLEAN addressing) noexcept
{ if (addressing != nullptr) { *addressing = FALSE; } return NDIS_STATUS_NOT_SUPPORTED; }
BOOLEAN check_for_hang(Adapter&) noexcept { return FALSE; }
NDIS_STATUS request_power(Adapter&, NDIS_DEVICE_POWER_STATE) noexcept { return NDIS_STATUS_ADAPTER_NOT_READY; }
void request_stop(Adapter& adapter, bool removed) noexcept
{
    InterlockedExchange(&adapter.stop_requested, 1);
    close_interrupt_admission(adapter, removed);
}
void shutdown_recovery(Adapter& adapter) noexcept { request_stop(adapter); }
bool halt_recovery(Adapter& adapter) noexcept
{ return stop_packet_path(adapter) == NDIS_STATUS_SUCCESS && retire_interrupt(adapter); }
bool retire_driver_recovery(DriverRecoveryRegistry&) noexcept { return true; }
} // namespace rtl8125::reactos
