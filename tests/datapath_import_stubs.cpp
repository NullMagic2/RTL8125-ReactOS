// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.datapath_import_stubs
Purpose: Supply genuine-SDK OS imports for the historical probe-only ABI suite.
Created: 2026-10-24
Architecture: All acquisition imports fail closed. Full production packet tests
use datapath_native_tests.cpp device/NDIS models, not these probe-only stubs.
*/
#include "../src/reactos/dma.hpp"
namespace {
NDIS_STATUS NTAPI dma_init(NDIS_HANDLE, BOOLEAN, ULONG) { return NDIS_STATUS_FAILURE; }
VOID NTAPI common(NDIS_HANDLE, ULONG, BOOLEAN, PVOID* output, PNDIS_PHYSICAL_ADDRESS address)
{ *output = nullptr; address->QuadPart = 0; }
VOID NTAPI common_free(NDIS_HANDLE, ULONG, BOOLEAN, PVOID, NDIS_PHYSICAL_ADDRESS) {}
VOID NTAPI packet_pool(PNDIS_STATUS status, PNDIS_HANDLE output, UINT, UINT)
{ *status = NDIS_STATUS_RESOURCES; *output = nullptr; }
VOID NTAPI buffer_pool(PNDIS_STATUS status, PNDIS_HANDLE output, UINT)
{ *status = NDIS_STATUS_RESOURCES; *output = nullptr; }
VOID NTAPI packet(PNDIS_STATUS status, PNDIS_PACKET* output, NDIS_HANDLE)
{ *status = NDIS_STATUS_RESOURCES; *output = nullptr; }
VOID NTAPI buffer(PNDIS_STATUS status, PNDIS_BUFFER* output, NDIS_HANDLE, PVOID, UINT)
{ *status = NDIS_STATUS_RESOURCES; *output = nullptr; }
VOID NTAPI free_packet(PNDIS_PACKET) {}
VOID NTAPI free_buffer(PMDL) {}
VOID NTAPI free_pool(NDIS_HANDLE) {}
PVOID NTAPI map(PMDL, KPROCESSOR_MODE, MEMORY_CACHING_TYPE, PVOID, ULONG, ULONG) { return nullptr; }
VOID NTAPI sleep(ULONG) {}
VOID NTAPI stall(ULONG) {}
ULONGLONG NTAPI interrupt_time() { return 0; }
ULONG NTAPI pci(NDIS_HANDLE, ULONG, ULONG, PVOID, ULONG) { return 0; }
}
namespace rtl8125::reactos {
bool read_mmio8(const DeviceResources&, ULONG, UCHAR& value) noexcept { value = 0; return false; }
bool read_mmio16(const DeviceResources&, ULONG, USHORT& value) noexcept { value = 0; return false; }
bool read_mmio32(const DeviceResources&, ULONG, ULONG& value) noexcept { value = 0; return false; }
bool write_mmio8(const DeviceResources&, ULONG, UCHAR) noexcept { return false; }
bool write_mmio16(const DeviceResources&, ULONG, USHORT) noexcept { return false; }
bool write_mmio32(const DeviceResources&, ULONG, ULONG) noexcept { return false; }
bool flush_posted_writes(const DeviceResources&) noexcept { return false; }
}
#define MOCK(name, bytes, implementation) \
    decltype(&name) import_##name __asm__("__imp__" #name "@" #bytes) = implementation
extern "C" {
MOCK(NdisMInitializeScatterGatherDma, 12, dma_init);
MOCK(NdisMAllocateSharedMemory, 20, common);
MOCK(NdisMFreeSharedMemory, 24, common_free);
MOCK(NdisAllocatePacketPool, 16, packet_pool);
MOCK(NdisAllocateBufferPool, 12, buffer_pool);
MOCK(NdisAllocatePacket, 12, packet);
MOCK(NdisAllocateBuffer, 20, buffer);
MOCK(NdisFreePacket, 4, free_packet);
MOCK(NdisFreePacketPool, 4, free_pool);
MOCK(NdisFreeBufferPool, 4, free_pool);
MOCK(IoFreeMdl, 4, free_buffer);
MOCK(MmMapLockedPagesSpecifyCache, 24, map);
MOCK(NdisMSleep, 4, sleep);
MOCK(KeStallExecutionProcessor, 4, stall);
MOCK(KeQueryInterruptTime, 0, interrupt_time);
MOCK(NdisReadPciSlotInformation, 20, pci);
MOCK(NdisWritePciSlotInformation, 20, pci);
}
