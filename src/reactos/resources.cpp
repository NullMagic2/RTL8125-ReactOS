// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.resources
Purpose: Query/map assigned NDIS resources and probe RTL8125 XID without writes.
Created: 2026-10-24
Architecture: Implements resources.hpp against real ReactOS NDIS/NT interfaces.
Derived BAR/probe/order policy: Linux rtl_init_one and rtl_pci_commit; THIRD_PARTY.txt.
*/
#include "resources.hpp"
#include "../core/resources.hpp"

namespace rtl8125::reactos {
namespace {

constexpr ULONG resource_pool_tag = 0x35524c52; // RLR5

// Bind portable CM byte parsing to actual SDK layouts, including the pointer-
// sized interrupt affinity that makes the AMD64 descriptor longer.
static_assert(FIELD_OFFSET(NDIS_RESOURCE_LIST, PartialDescriptors) == 8);
static_assert(FIELD_OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Memory.Start) == 4);
static_assert(FIELD_OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Memory.Length) == 12);
static_assert(FIELD_OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Interrupt.Level) == 4);
static_assert(FIELD_OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Interrupt.Vector) == 8);
static_assert(FIELD_OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Interrupt.Affinity) == 12);
static_assert(sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 12 + sizeof(KAFFINITY));

bool valid_access(const DeviceResources& resources, ULONG offset, ULONG width) noexcept
{
    // NdisMMapIoSpace returns a page-aligned mapping. The portable range check
    // establishes natural offset alignment before typed register pointer casts;
    // the caller keeps the complete mapping alive for the duration of access.
    return resources.mapped_address != nullptr &&
           valid_mmio_access(resources.mapped_length, offset, width);
}

NDIS_STATUS query_assigned_resources(NDIS_HANDLE configuration_context,
                                    const PciRegisterBar& bar,
                                    AssignedResources& assigned) noexcept
{
    UINT required_size = 0;
    NDIS_STATUS status = NDIS_STATUS_FAILURE;
    NdisMQueryAdapterResources(&status, configuration_context, nullptr, &required_size);

    if (status != NDIS_STATUS_RESOURCES || required_size < 8 ||
        required_size > maximum_resource_list_size) {
        return NDIS_STATUS_RESOURCES;
    }

    PVOID allocation = nullptr;
    status = NdisAllocateMemoryWithTag(&allocation, required_size, resource_pool_tag);

    if (status != NDIS_STATUS_SUCCESS) {
        return status;
    }

    if (allocation == nullptr) {
        return NDIS_STATUS_RESOURCES;
    }

    const auto list = static_cast<PNDIS_RESOURCE_LIST>(allocation);
    const auto allocated_size = required_size;
    NdisMQueryAdapterResources(&status, configuration_context, list, &required_size);

    if (status == NDIS_STATUS_SUCCESS &&
        (required_size > allocated_size ||
         !decode_assigned_resources(reinterpret_cast<const std::uint8_t*>(list),
                                     required_size,
                                     sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR),
                                     bar, assigned))) {
        status = NDIS_STATUS_RESOURCES;
    }

    NdisFreeMemory(list, allocated_size, 0);
    return status;
}

} // namespace

NDIS_STATUS acquire_resources(NDIS_HANDLE adapter_handle,
                              NDIS_HANDLE configuration_context,
                              DeviceResources& resources) noexcept
{
    if (resources.mapped_address != nullptr) {
        return NDIS_STATUS_FAILURE;
    }

    resources = {};

    if (adapter_handle == nullptr || configuration_context == nullptr) {
        return NDIS_STATUS_FAILURE;
    }

    std::uint8_t config[pci_config_snapshot_size]{};
    const auto bytes_read = NdisReadPciSlotInformation(adapter_handle, 0, 0, config,
                                                       sizeof(config));
    PciRegisterBar bar{};

    if (bytes_read != sizeof(config) || !decode_register_bar(config, sizeof(config), bar)) {
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    AssignedResources assigned{};
    auto status = query_assigned_resources(configuration_context, bar, assigned);

    if (status != NDIS_STATUS_SUCCESS) {
        return status;
    }

    DeviceResources candidate{};
    candidate.physical_address.LowPart = static_cast<ULONG>(assigned.address);
    candidate.physical_address.HighPart = static_cast<LONG>(assigned.address >> 32);
    candidate.mapped_length = assigned.length;
    candidate.interrupt_vector = assigned.interrupt_vector;
    candidate.interrupt_level = assigned.interrupt_level;
    candidate.interrupt_affinity = static_cast<KAFFINITY>(assigned.interrupt_affinity);
    candidate.interrupt_flags = assigned.interrupt_flags;
    candidate.interrupt_shared = assigned.interrupt_shared ? TRUE : FALSE;
    candidate.bar_index = bar.index;
    candidate.pci_revision = bar.revision;

    PVOID mapping = nullptr;
    status = NdisMMapIoSpace(&mapping, adapter_handle, candidate.physical_address,
                             candidate.mapped_length);
    candidate.mapped_address = static_cast<PUCHAR>(mapping);

    if (status != NDIS_STATUS_SUCCESS || candidate.mapped_address == nullptr) {
        // Even anomalous failure-with-pointer must retire the returned mapping.
        release_resources(adapter_handle, candidate);
        return status == NDIS_STATUS_SUCCESS ? NDIS_STATUS_RESOURCES : status;
    }

    if (!read_mmio32(candidate, tx_config_offset, candidate.tx_config)) {
        release_resources(adapter_handle, candidate);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    candidate.chip = identify_chip(realtek_vendor_id, rtl8125_device_id,
                                    candidate.tx_config);

    if (candidate.chip.status != ChipStatus::supported) {
        release_resources(adapter_handle, candidate);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    resources = candidate;
    return NDIS_STATUS_SUCCESS;
}

void release_resources(NDIS_HANDLE adapter_handle, DeviceResources& resources) noexcept
{
    if (resources.mapped_address != nullptr) {
        NdisMUnmapIoSpace(adapter_handle, resources.mapped_address, resources.mapped_length);
    }

    resources = {};
}

bool read_mmio8(const DeviceResources& resources, ULONG offset, UCHAR& value) noexcept
{
    value = 0;

    if (!valid_access(resources, offset, 1)) {
        return false;
    }

    value = READ_REGISTER_UCHAR(resources.mapped_address + offset);
    return true;
}

bool read_mmio16(const DeviceResources& resources, ULONG offset, USHORT& value) noexcept
{
    value = 0;

    if (!valid_access(resources, offset, 2)) {
        return false;
    }

    value = READ_REGISTER_USHORT(reinterpret_cast<PUSHORT>(resources.mapped_address + offset));
    return true;
}

bool read_mmio32(const DeviceResources& resources, ULONG offset, ULONG& value) noexcept
{
    value = 0;

    if (!valid_access(resources, offset, 4)) {
        return false;
    }

    value = READ_REGISTER_ULONG(reinterpret_cast<PULONG>(resources.mapped_address + offset));
    return true;
}

bool write_mmio8(const DeviceResources& resources, ULONG offset, UCHAR value) noexcept
{
    if (!valid_access(resources, offset, 1)) {
        return false;
    }

    WRITE_REGISTER_UCHAR(resources.mapped_address + offset, value);
    return true;
}

bool write_mmio16(const DeviceResources& resources, ULONG offset, USHORT value) noexcept
{
    if (!valid_access(resources, offset, 2)) {
        return false;
    }

    WRITE_REGISTER_USHORT(reinterpret_cast<PUSHORT>(resources.mapped_address + offset), value);
    return true;
}

bool write_mmio32(const DeviceResources& resources, ULONG offset, ULONG value) noexcept
{
    if (!valid_access(resources, offset, 4)) {
        return false;
    }

    WRITE_REGISTER_ULONG(reinterpret_cast<PULONG>(resources.mapped_address + offset), value);
    return true;
}

bool flush_posted_writes(const DeviceResources& resources) noexcept
{
    UCHAR command{};
    return read_mmio8(resources, 0x37, command) && command != 0xff;
}

} // namespace rtl8125::reactos
