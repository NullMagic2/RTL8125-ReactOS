// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.recovery_cpu
Purpose: Complete driver KDPC epilogues before adapter or image retirement.
Created: 2026-10-24
Architecture: Genuine ReactOS i386 scheduler affinity rendezvous.
*/
#include "resources.hpp"
#include "recovery_cpu.hpp"
namespace rtl8125::reactos {
bool rendezvous_adapter_processors() noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return false; }
    const KAFFINITY active = KeQueryActiveProcessors();
    if (active == 0) { return false; }
    bool success = true;
    for (ULONG processor = 0; processor < sizeof(KAFFINITY) * 8; ++processor) {
        const auto mask = static_cast<KAFFINITY>(1) << processor;
        if ((active & mask) == 0) { continue; }
        // The matched implementation changes thread affinity under dispatcher
        // lock. Lowering IRQL switches CPUs before returning to PASSIVE code.
        KeSetSystemAffinityThread(mask);
        if (KeGetCurrentProcessorNumber() != processor) { success = false; break; }
    }
    KeRevertToUserAffinityThread();
    return success;
}
}
