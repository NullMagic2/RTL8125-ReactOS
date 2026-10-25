// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.datapath_adapter
Purpose: Bind production adapter admission, packets and safe owner retirement.
Created: 2026-10-24
Architecture: Actual M03 state and explicit M07/M08 prerequisite orchestration.
*/
#include "adapter.hpp"

namespace rtl8125::reactos {
namespace {
bool prerequisites_present(const PacketPrerequisites& prerequisites) noexcept
{
    return prerequisites.interrupt_ready != nullptr && prerequisites.activate != nullptr &&
        prerequisites.retire_cpu_access != nullptr && prerequisites.defer_service != nullptr;
}
bool hardware_prepared(const Adapter& adapter) noexcept
{
    return adapter.hardware.device.initialized && !adapter.hardware.device.running &&
        !adapter.hardware.device.recovery_required &&
        adapter.hardware.device.version == adapter.resources.chip.version &&
        adapter.resources.chip.status == ChipStatus::supported;
}
}
NDIS_STATUS prepare_packet_path(Adapter& adapter, const PacketPrerequisites& prerequisites) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || adapter.handle == nullptr ||
        adapter.servicing || adapter.datapath.adapter != nullptr || adapter.dma.allocated() ||
        (adapter.phase != AdapterPhase::incomplete && adapter.phase != AdapterPhase::allocated) ||
        !hardware_prepared(adapter) || !prerequisites_present(prerequisites) ||
        !prerequisites.interrupt_ready(prerequisites.context)) { return NDIS_STATUS_ADAPTER_NOT_READY; }
    if (acquire_dma(adapter.handle, adapter.dma) != DmaStatus::ok) { return NDIS_STATUS_RESOURCES; }
    const auto status = acquire_datapath(adapter.handle, adapter.dma, adapter.resources,
        adapter.hardware.device.permanent_address, adapter.datapath);
    if (status != NDIS_STATUS_SUCCESS) {
        release_dma(adapter.dma);
        return status;
    }
    adapter.packet_prerequisites = prerequisites;
    adapter.packet_filter = 0;
    adapter.link_up = false;
    adapter.link_speed_100bps = 0;
    adapter.phase = AdapterPhase::prepared;
    return NDIS_STATUS_SUCCESS;
}
NDIS_STATUS activate_packet_path(Adapter& adapter) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || adapter.phase != AdapterPhase::prepared ||
        adapter.servicing || !hardware_prepared(adapter) ||
        !prerequisites_present(adapter.packet_prerequisites) ||
        !adapter.packet_prerequisites.interrupt_ready(adapter.packet_prerequisites.context)) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }
    auto status = start_datapath(adapter.datapath);
    if (status == NDIS_STATUS_SUCCESS &&
        adapter.packet_prerequisites.activate(adapter.packet_prerequisites.context, adapter.dma) &&
        adapter.hardware.device.initialized && adapter.hardware.device.running &&
        !adapter.hardware.device.recovery_required) {
        adapter.phase = AdapterPhase::running;
        return NDIS_STATUS_SUCCESS;
    }
    // Ring publication and partial platform activation cannot be rolled back.
    // Attempt real retirement; the retained phase keeps ownership on failure.
    stop_packet_path(adapter);
    return NDIS_STATUS_FAILURE;
}
NDIS_STATUS stop_packet_path(Adapter& adapter) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || adapter.servicing) { return NDIS_STATUS_FAILURE; }
    adapter.phase = AdapterPhase::quiescing;
    adapter.link_up = false;
    adapter.link_speed_100bps = 0;
    stop_datapath(adapter.datapath);
    if (adapter.dma.device_accessible()) {
        if (!prerequisites_present(adapter.packet_prerequisites) ||
            retire_dma(adapter.dma, adapter.resources,
                adapter.packet_prerequisites.retire_cpu_access,
                adapter.packet_prerequisites.context) != DmaStatus::ok) {
            adapter.phase = AdapterPhase::retained;
            return NDIS_STATUS_FAILURE;
        }
        // The DMA retirement reset invalidates the previous MAC/PHY setup.
        adapter.hardware.device.running = false;
        adapter.hardware.device.initialized = false;
        adapter.hardware.device.recovery_required = true;
    } else if (adapter.packet_prerequisites.retire_cpu_access != nullptr &&
               !adapter.packet_prerequisites.retire_cpu_access(adapter.packet_prerequisites.context)) {
        adapter.phase = AdapterPhase::retained;
        return NDIS_STATUS_FAILURE;
    }
    if (adapter.datapath.adapter != nullptr &&
        abort_transmits(adapter.datapath, NDIS_STATUS_FAILURE) != NDIS_STATUS_SUCCESS) {
        adapter.phase = AdapterPhase::retained;
        return NDIS_STATUS_FAILURE;
    }
    if (release_datapath(adapter.datapath) != NDIS_STATUS_SUCCESS ||
        release_dma(adapter.dma) != DmaStatus::ok) {
        adapter.phase = AdapterPhase::retained;
        return NDIS_STATUS_FAILURE;
    }
    adapter.packet_prerequisites = {};
    adapter.packet_filter = 0;
    adapter.phase = AdapterPhase::incomplete;
    return NDIS_STATUS_SUCCESS;
}
bool report_packet_link(Adapter& adapter, const hardware::LinkState& link) noexcept
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL || adapter.phase != AdapterPhase::running ||
        !adapter.hardware.device.initialized || !adapter.hardware.device.running ||
        adapter.hardware.device.recovery_required) { return false; }
    if (link.up && (!link.autoneg_complete ||
        (link.speed_mbps != 10 && link.speed_mbps != 100 && link.speed_mbps != 1000 && link.speed_mbps != 2500))) {
        return false;
    }
    adapter.link_up = link.up;
    adapter.link_speed_100bps = link.up ? link.speed_mbps * 10000 : 0;
    adapter.full_duplex = link.up && link.full_duplex;
    return true;
}
} // namespace rtl8125::reactos
