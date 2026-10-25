// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.hardware_io
Purpose: Adapt real checked MMIO/PCI access and bounded native register sessions.
Created: 2026-10-24
Architecture: Shared RegisterIo bridge; firmware file ownership remains separate.
*/
#include "hardware_init.hpp"

namespace rtl8125::reactos {
namespace {
bool claim(void* context) noexcept
{
    auto& access = *static_cast<HardwareAccess*>(context);
    return KeGetCurrentIrql() == PASSIVE_LEVEL && access.resources != nullptr &&
        access.resources->mapped_address != nullptr &&
        InterlockedCompareExchange(&access.claimed, 1, 0) == 0;
}

void release(void* context) noexcept
{
    auto& access = *static_cast<HardwareAccess*>(context);
    InterlockedExchange(&access.claimed, 0);
}

bool read(void* context, std::uint32_t offset, unsigned width, std::uint32_t& value) noexcept
{
    value = 0;
    const auto& access = *static_cast<HardwareAccess*>(context);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || access.claimed != 1 || access.resources == nullptr) {
        return false;
    }
    if (width == 1) {
        UCHAR byte{};
        if (!read_mmio8(*access.resources, offset, byte)) { return false; }
        value = byte;
        return true;
    }
    if (width == 2) {
        USHORT word{};
        if (!read_mmio16(*access.resources, offset, word)) { return false; }
        value = word;
        return true;
    }
    ULONG dword{};
    if (width != 4 || !read_mmio32(*access.resources, offset, dword)) { return false; }
    value = dword;
    return true;
}

bool write(void* context, std::uint32_t offset, unsigned width, std::uint32_t value) noexcept
{
    const auto& access = *static_cast<HardwareAccess*>(context);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || access.claimed != 1 || access.resources == nullptr) {
        return false;
    }
    if (width == 1 && value <= 0xff) {
        return write_mmio8(*access.resources, offset, static_cast<UCHAR>(value));
    }
    if (width == 2 && value <= 0xffff) {
        return write_mmio16(*access.resources, offset, static_cast<USHORT>(value));
    }
    return width == 4 && write_mmio32(*access.resources, offset, value);
}

bool delay(void*, std::uint32_t microseconds) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || microseconds > 2000000) {
        return false;
    }
    // ReactOS NdisMSleep always waits on a kernel timer; using it for the
    // Linux 1/10/25/100-us udelay polls would round every access to a tick.
    // Bound each processor stall to 50 us; longer waits yield at PASSIVE_LEVEL.
    if (microseconds <= 100) {
        while (microseconds != 0) {
            const auto interval = microseconds > 50 ? 50u : microseconds;
            KeStallExecutionProcessor(interval);
            microseconds -= interval;
        }
    } else {
        NdisMSleep(microseconds);
    }
    return true;
}

std::uint64_t now(void*) noexcept
{
    return KeQueryInterruptTime() / 10;
}

bool pci_read(void* context, std::uint32_t offset, unsigned width, std::uint32_t& value) noexcept
{
    value = 0;
    auto& owner = *static_cast<HardwareAccess*>(context);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || owner.claimed != 1 ||
        owner.adapter_handle == nullptr || (width != 1 && width != 2 && width != 4) ||
        offset > 4096 - width) { return false; }
    UCHAR bytes[4]{};
    if (NdisReadPciSlotInformation(owner.adapter_handle, 0, offset, bytes, width) != width) {
        return false;
    }
    for (unsigned index = 0; index < width; ++index) { value |= std::uint32_t(bytes[index]) << (index * 8); }
    return true;
}

bool pci_write(void* context, std::uint32_t offset, unsigned width, std::uint32_t value) noexcept
{
    auto& owner = *static_cast<HardwareAccess*>(context);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || owner.claimed != 1 ||
        owner.adapter_handle == nullptr || (width != 1 && width != 2 && width != 4) ||
        offset > 4096 - width) { return false; }
    UCHAR bytes[4]{};
    for (unsigned index = 0; index < width; ++index) { bytes[index] = static_cast<UCHAR>(value >> (index * 8)); }
    return NdisWritePciSlotInformation(owner.adapter_handle, 0, offset, bytes, width) == width;
}

}

hardware::RegisterIo hardware_register_io(HardwareAccess& owner) noexcept
{
    return {&owner, claim, release, read, write, delay, now, pci_read, pci_write, owner.pci_function};
}

} // namespace rtl8125::reactos
