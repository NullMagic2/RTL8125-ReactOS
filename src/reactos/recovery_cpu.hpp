// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.recovery_cpu
Purpose: Isolate actual CPU-affinity callback code retirement from OS mocks.
Created: 2026-10-24
Architecture: PASSIVE NT primitive, callable only after all driver KDPCs close.
*/
#pragma once
namespace rtl8125::reactos {
// PASSIVE, caller has closed every scheduler and drained private DPC references.
// A PASSIVE visit to every active processor follows the return of any old DPC
// epilogue on that processor. CPU hotplug is unsupported by this i386 NT target.
bool rendezvous_adapter_processors() noexcept;
}
