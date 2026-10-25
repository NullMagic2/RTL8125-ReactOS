// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.miniport
Purpose: Register a real NDIS 5.1 ABI and perform a fail-closed device probe.
Created: 2026-10-24
Architecture: Thin native callbacks; resources and portable bounds have owners.
*/
#include "adapter.hpp"

#include <new>

namespace {

using rtl8125::InformationStatus;
using rtl8125::reactos::Adapter;
using rtl8125::reactos::AdapterPhase;

constexpr ULONG adapter_tag = 0x35323152; // R125 in little-endian pool diagnostics.
const char registration_extension_key{};

struct DriverRegistration {
    NDIS_HANDLE wrapper;
    rtl8125::reactos::DriverRecoveryRegistry recovery;
};

// Public packet capabilities have real implementations. Wake/power/offload
// capabilities remain absent; actual media readiness comes from prerequisites.
constexpr NDIS_OID supported_oids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION, OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_DRIVER_VERSION,
    OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_PHYSICAL_MEDIUM,
    OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST, OID_802_3_MAXIMUM_LIST_SIZE,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR, OID_GEN_RCV_NO_BUFFER,
    OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS, OID_PNP_CAPABILITIES,
    OID_PNP_QUERY_POWER, OID_PNP_SET_POWER
};

NDIS_STATUS information_status(InformationStatus status) noexcept
{
    switch (status) {
    case InformationStatus::success: return NDIS_STATUS_SUCCESS;
    case InformationStatus::short_buffer: return NDIS_STATUS_INVALID_LENGTH;
    case InformationStatus::invalid_length: return NDIS_STATUS_INVALID_LENGTH;
    case InformationStatus::invalid_buffer: return NDIS_STATUS_INVALID_DATA;
    case InformationStatus::invalid_value: return NDIS_STATUS_INVALID_DATA;
    case InformationStatus::unsupported: return NDIS_STATUS_NOT_SUPPORTED;
    }

    return NDIS_STATUS_FAILURE;
}

// PASSIVE_LEVEL, exactly once per allocated Adapter. acquire_resources handles
// its partial failures too; releasing an empty DeviceResources is idempotent.
void destroy_adapter(Adapter* adapter) noexcept
{
    if (adapter == nullptr) {
        return;
    }
    if (adapter->shutdown_registered) {
        NdisMDeregisterAdapterShutdownHandler(adapter->handle);
        adapter->shutdown_registered = false;
    }

    if (!rtl8125::reactos::halt_recovery(*adapter)) {
        DbgPrint("RTL8125: unsafe packet retirement; adapter and mapping retained\n");
        return;
    }
    adapter->phase = AdapterPhase::stopping;
    if (!rtl8125::reactos::retire_interrupt(*adapter)) {
        adapter->phase = AdapterPhase::retained;
        return;
    }
    rtl8125::reactos::release_resources(adapter->handle, adapter->resources);
    adapter->~Adapter();
    NdisFreeMemory(adapter, sizeof(Adapter), 0);
}

} // namespace

// PASSIVE_LEVEL after I/O/PnP has retired every adapter. Failed Initialize
// retains no adapter or asynchronous work in this milestone. The driver-object
// extension lives until the I/O manager destroys the driver object; no global
// wrapper handle or independently freed extension can outlive its owner.
extern "C" VOID NTAPI rtl8125_unload(PDRIVER_OBJECT driver_object)
{
    auto* registration = static_cast<DriverRegistration*>(IoGetDriverObjectExtension(
        driver_object, const_cast<char*>(&registration_extension_key)));

    if (registration != nullptr && registration->wrapper != nullptr) {
        if (!rtl8125::reactos::retire_driver_recovery(registration->recovery)) {
            DbgPrint("RTL8125: driver retirement incomplete; image ownership retained\n");
            return;
        }
        const auto wrapper = registration->wrapper;
        registration->wrapper = nullptr;
        DbgPrint("RTL8125: wrapper unload begin\n");
        NdisTerminateWrapper(wrapper, nullptr);
        DbgPrint("RTL8125: wrapper unload complete\n");
    }
}

// Initialize/Halt run at PASSIVE_LEVEL. No hardware state is changed, so failed
// initialization can unwind mappings immediately. Success remains forbidden
// until MAC/PHY, DMA and packet/interrupt lifetimes exist in later milestones.
extern "C" NDIS_STATUS NTAPI rtl8125_initialize(
    PNDIS_STATUS open_error, PUINT selected_medium, PNDIS_MEDIUM media,
    UINT media_count, NDIS_HANDLE adapter_handle, NDIS_HANDLE configuration)
{
    if (open_error == nullptr || selected_medium == nullptr) {
        return NDIS_STATUS_FAILURE;
    }

    *open_error = NDIS_STATUS_FAILURE;
    *selected_medium = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        (media_count != 0 && media == nullptr)) {
        return NDIS_STATUS_FAILURE;
    }

    UINT index{};

    for (; index < media_count; ++index) {
        if (media[index] == NdisMedium802_3) {
            break;
        }
    }

    if (index == media_count) {
        *open_error = NDIS_STATUS_UNSUPPORTED_MEDIA;
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    }

    *selected_medium = index;
    PVOID storage{};
    auto status = NdisAllocateMemoryWithTag(&storage, sizeof(Adapter), adapter_tag);

    if (status != NDIS_STATUS_SUCCESS || storage == nullptr) {
        *open_error = NDIS_STATUS_RESOURCES;
        return NDIS_STATUS_RESOURCES;
    }

    auto* adapter = new (storage) Adapter{};
    adapter->handle = adapter_handle;
    adapter->hardware_access.adapter_handle = adapter_handle;
    adapter->phase = AdapterPhase::probing;

    // Operational attributes create the actual NDIS DMA adapter prerequisite.
    // The default probe never requests PCI command or bus-master changes.
#if defined(RTL8125_OPERATIONAL) && RTL8125_OPERATIONAL
    NdisMSetAttributesEx(adapter_handle, adapter, 2, NDIS_ATTRIBUTE_BUS_MASTER, NdisInterfacePci);
#else
    NdisMSetAttributesEx(adapter_handle, adapter, 0, 0, NdisInterfacePci);
#endif
    adapter->shutdown_registered = true;
    status = rtl8125::reactos::acquire_resources(adapter_handle, configuration,
                                               adapter->resources);

    if (status == NDIS_STATUS_SUCCESS) {
        adapter->phase = AdapterPhase::incomplete;
        DbgPrint("RTL8125: probe begin v1\n");
        DbgPrint("RTL8125: rev %02lx BAR%lu address %08lx:%08lx length %08lx\n",
                 static_cast<ULONG>(adapter->resources.pci_revision),
                 static_cast<ULONG>(adapter->resources.bar_index),
                 static_cast<ULONG>(adapter->resources.physical_address.HighPart),
                 adapter->resources.physical_address.LowPart,
                 adapter->resources.mapped_length);
        DbgPrint("RTL8125: IRQ vector %lu level %lu flags %04lx TxConfig %08lx\n",
                 adapter->resources.interrupt_vector,
                 adapter->resources.interrupt_level,
                 static_cast<ULONG>(adapter->resources.interrupt_flags),
                 adapter->resources.tx_config);
#if defined(RTL8125_OPERATIONAL) && RTL8125_OPERATIONAL
        DbgPrint("RTL8125: operational XID %03lx, MAC family %lu; starting owned lifecycle\n",
                 static_cast<ULONG>(adapter->resources.chip.xid),
                 static_cast<ULONG>(adapter->resources.chip.version));
        PDEVICE_OBJECT physical{}, functional{};
        NdisMGetDeviceProperty(adapter_handle, &physical, &functional, nullptr, nullptr, nullptr);
        ULONG address{}, returned{};
        auto* registration = functional != nullptr && functional->DriverObject != nullptr ?
            static_cast<DriverRegistration*>(IoGetDriverObjectExtension(functional->DriverObject,
                const_cast<char*>(&registration_extension_key))) : nullptr;
        if (physical == nullptr || registration == nullptr ||
            !NT_SUCCESS(IoGetDeviceProperty(physical, DevicePropertyAddress, sizeof(address), &address, &returned)) ||
            returned != sizeof(address) || (address & 0xffff) > 7 || (address >> 16) > 31) {
            status = NDIS_STATUS_INVALID_DATA;
        } else {
            adapter->hardware_access.pci_function = static_cast<UCHAR>(address & 0xffff);
            status = rtl8125::reactos::start_recovery(*adapter, registration->recovery);
        }
        if (status == NDIS_STATUS_SUCCESS) {
            *open_error = NDIS_STATUS_SUCCESS;
            DbgPrint("RTL8125: operational adapter initialized\n");
            return NDIS_STATUS_SUCCESS;
        }
#else
        DbgPrint("RTL8125: probe XID %03lx, MAC family %lu; datapath incomplete\n",
                 static_cast<ULONG>(adapter->resources.chip.xid),
                 static_cast<ULONG>(adapter->resources.chip.version));
        status = NDIS_STATUS_NOT_SUPPORTED;
#endif
    }

    DbgPrint("RTL8125: probe end status %08lx\n", static_cast<ULONG>(status));

    // NDIS does not call Halt for failed Initialize. This owns all cleanup.
    destroy_adapter(adapter);
    *open_error = status;
    return status;
}

extern "C" VOID NTAPI rtl8125_halt(NDIS_HANDLE context)
{
    destroy_adapter(static_cast<Adapter*>(context));
}

// Serialized callback, <= DISPATCH_LEVEL. Active packet queries use the real
// adapter-owned datapath; absent hardware prerequisites remain disconnected.
// General statistics retain64/32-bit compatible replies without truncation.
extern "C" NDIS_STATUS NTAPI rtl8125_query(
    NDIS_HANDLE context, NDIS_OID oid, PVOID buffer, ULONG capacity,
    PULONG bytes_written, PULONG bytes_needed)
{
    if (bytes_written == nullptr || bytes_needed == nullptr) {
        return NDIS_STATUS_FAILURE;
    }

    *bytes_written = 0;
    *bytes_needed = 0;
    const auto* adapter = static_cast<const Adapter*>(context);
    if (oid == OID_PNP_CAPABILITIES) {
        NDIS_PNP_CAPABILITIES capabilities{};
        const auto result = rtl8125::copy_information(buffer, capacity, &capabilities, sizeof(capabilities));
        *bytes_written = static_cast<ULONG>(result.transferred);
        *bytes_needed = static_cast<ULONG>(result.needed);
        return information_status(result.status);
    }
    if (oid == OID_PNP_QUERY_POWER) {
        *bytes_needed = sizeof(NDIS_DEVICE_POWER_STATE);
        if (adapter == nullptr || buffer == nullptr || capacity != sizeof(NDIS_DEVICE_POWER_STATE)) {
            return NDIS_STATUS_INVALID_LENGTH;
        }
        NDIS_DEVICE_POWER_STATE state{};
        NdisMoveMemory(&state, buffer, sizeof(state));
        if (state < NdisDeviceStateD0 || state > NdisDeviceStateD3) { return NDIS_STATUS_INVALID_DATA; }
        *bytes_written = sizeof(state); *bytes_needed = 0;
        return NDIS_STATUS_SUCCESS;
    }
    rtl8125::reactos::AdapterDispatchGuard dispatch(const_cast<Adapter*>(adapter));
    if (!dispatch || InterlockedCompareExchange(&const_cast<Adapter*>(adapter)->stop_requested, 0, 0) != 0) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }

    if (adapter == nullptr || adapter->phase == AdapterPhase::stopping) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }

    const bool packet_owned = adapter->datapath.adapter != nullptr;
    const bool active = adapter->phase == AdapterPhase::running && adapter->datapath.running &&
        !adapter->datapath.faulted && adapter->hardware.device.running &&
        !adapter->hardware.device.recovery_required &&
        (!adapter->interrupts.registered || rtl8125::reactos::interrupt_registered(*adapter));
    if (packet_owned && (oid == OID_GEN_CURRENT_PACKET_FILTER ||
        oid == OID_802_3_MULTICAST_LIST || oid == OID_802_3_MAXIMUM_LIST_SIZE ||
        oid == OID_802_3_CURRENT_ADDRESS || oid == OID_802_3_PERMANENT_ADDRESS)) {
        return rtl8125::reactos::query_datapath_information(adapter->datapath, oid,
            buffer, capacity, bytes_written, bytes_needed);
    }
    ULONG value{};
    ULONGLONG counter{};
    bool counter_query{};
    USHORT driver_version = 0x0501;
    const void* source = &value;
    std::size_t length = sizeof(value);
    constexpr char description[] = "RTL8125 ReactOS probe; datapath incomplete";
    constexpr char active_description[] = "RTL8125 ReactOS Ethernet";

    switch (oid) {
    case OID_GEN_SUPPORTED_LIST:
        source = supported_oids;
        length = sizeof(supported_oids);
        break;
    case OID_GEN_HARDWARE_STATUS: value = active ? NdisHardwareStatusReady : NdisHardwareStatusNotReady; break;
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE: value = NdisMedium802_3; break;
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_MAXIMUM_FRAME_SIZE:
        value = rtl8125::maximum_frame_size - rtl8125::ethernet_header_size;
        break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE: value = rtl8125::maximum_frame_size; break;
    case OID_GEN_VENDOR_ID:
        value = packet_owned ? (static_cast<ULONG>(adapter->datapath.station[0]) << 16) |
            (static_cast<ULONG>(adapter->datapath.station[1]) << 8) | adapter->datapath.station[2] : 0xffffff;
        break;
    case OID_GEN_VENDOR_DESCRIPTION:
        source = packet_owned ? active_description : description;
        length = packet_owned ? sizeof(active_description) : sizeof(description);
        break;
    case OID_GEN_CURRENT_PACKET_FILTER: value = adapter->packet_filter; break;
    case OID_GEN_CURRENT_LOOKAHEAD: value = adapter->lookahead; break;
    case OID_GEN_DRIVER_VERSION:
        source = &driver_version;
        length = sizeof(driver_version);
        break;
    case OID_GEN_VENDOR_DRIVER_VERSION: value = 1; break;
    case OID_GEN_MEDIA_CONNECT_STATUS:
        value = active && adapter->link_up ? NdisMediaStateConnected : NdisMediaStateDisconnected;
        break;
    case OID_GEN_PHYSICAL_MEDIUM: value = NdisPhysicalMediumUnspecified; break;
    case OID_802_3_PERMANENT_ADDRESS:
    case OID_802_3_CURRENT_ADDRESS: return NDIS_STATUS_ADAPTER_NOT_READY;
    case OID_802_3_MULTICAST_LIST:
        source = nullptr;
        length = 0;
        break;
    case OID_GEN_LINK_SPEED: value = active && adapter->link_up ? adapter->link_speed_100bps : 0; break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE:
    case OID_GEN_RECEIVE_BUFFER_SPACE:
        value = packet_owned ? rtl8125::reactos::dma_ring_count * rtl8125::reactos::dma_packet_size : 0;
        break;
    case OID_GEN_MAXIMUM_SEND_PACKETS: value = packet_owned ? 1 : 0; break;
    case OID_GEN_MAC_OPTIONS:
        value = packet_owned ? NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
            NDIS_MAC_OPTION_RECEIVE_SERIALIZED | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND : 0;
        if (active && adapter->full_duplex) { value |= NDIS_MAC_OPTION_FULL_DUPLEX; }
        break;
    case OID_802_3_MAXIMUM_LIST_SIZE: break;
    case OID_GEN_XMIT_OK:
    case OID_GEN_RCV_OK:
    case OID_GEN_XMIT_ERROR:
    case OID_GEN_RCV_ERROR:
    case OID_GEN_RCV_NO_BUFFER:
        counter_query = true;
        if (packet_owned) {
            const auto& counts = adapter->datapath.counters;
            if (oid == OID_GEN_XMIT_OK) { counter = counts.tx_completed; }
            if (oid == OID_GEN_RCV_OK) { counter = counts.rx_indicated; }
            if (oid == OID_GEN_XMIT_ERROR) { counter = counts.tx_rejected + counts.tx_aborted; }
            if (oid == OID_GEN_RCV_ERROR) { counter = counts.rx_invalid; }
            // Fixed RX storage cannot exhaust after startup. NDIS wrapper
            // allocation failures are not observable as miniport buffers.
        }
        break;
    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS: break;
    default: return NDIS_STATUS_NOT_SUPPORTED;
    }

    const auto result = counter_query ?
        rtl8125::copy_counter_information(buffer, capacity, counter) :
        rtl8125::copy_information(buffer, capacity, source, length);
    *bytes_written = static_cast<ULONG>(result.transferred);
    *bytes_needed = static_cast<ULONG>(result.needed);
    return information_status(result.status);
}

// <= DISPATCH_LEVEL. Prepared/active adapters route real filter/list OIDs to
// the owned packet path. The read-only probe retains its conservative empty state.
extern "C" NDIS_STATUS NTAPI rtl8125_set(
    NDIS_HANDLE context, NDIS_OID oid, PVOID buffer, ULONG length,
    PULONG bytes_read, PULONG bytes_needed)
{
    if (bytes_read == nullptr || bytes_needed == nullptr) {
        return NDIS_STATUS_FAILURE;
    }

    *bytes_read = 0;
    *bytes_needed = 0;
    auto* adapter = static_cast<Adapter*>(context);
    if (oid == OID_PNP_SET_POWER) {
        *bytes_needed = sizeof(NDIS_DEVICE_POWER_STATE);
        if (adapter == nullptr || buffer == nullptr || length != sizeof(NDIS_DEVICE_POWER_STATE)) {
            return NDIS_STATUS_INVALID_LENGTH;
        }
        NDIS_DEVICE_POWER_STATE state{};
        NdisMoveMemory(&state, buffer, sizeof(state));
        // Publish request byte counts before signaling the passive worker.
        *bytes_read = sizeof(state); *bytes_needed = 0;
        const auto status = rtl8125::reactos::request_power(*adapter, state);
        if (status != NDIS_STATUS_PENDING) { *bytes_read = 0; }
        return status;
    }
    rtl8125::reactos::AdapterDispatchGuard dispatch(adapter);
    if (!dispatch || InterlockedCompareExchange(&adapter->stop_requested, 0, 0) != 0 ||
        (adapter->interrupts.registered && !rtl8125::reactos::interrupt_registered(*adapter))) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }


    if (adapter == nullptr || adapter->phase == AdapterPhase::stopping ||
        adapter->phase == AdapterPhase::quiescing || adapter->phase == AdapterPhase::retained) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }

    if (adapter->datapath.adapter != nullptr &&
        (oid == OID_GEN_CURRENT_PACKET_FILTER || oid == OID_802_3_MULTICAST_LIST)) {
        const auto status = rtl8125::reactos::set_datapath_information(adapter->datapath,
            oid, buffer, length, bytes_read, bytes_needed);
        if (status == NDIS_STATUS_SUCCESS) { adapter->packet_filter = adapter->datapath.filter.flags; }
        return status;
    }
    rtl8125::InformationResult result{InformationStatus::unsupported, 0, 0};

    switch (oid) {
    case OID_GEN_CURRENT_PACKET_FILTER:
        result = rtl8125::set_inactive_packet_filter(buffer, length,
                                                    adapter->packet_filter);
        break;
    case OID_GEN_CURRENT_LOOKAHEAD:
        result = rtl8125::set_lookahead(buffer, length, adapter->lookahead);
        break;
    case OID_802_3_MULTICAST_LIST:
        if (length == 0) {
            result = {InformationStatus::success, 0, 0};
        } else if (length % 6 != 0) {
            result = {InformationStatus::invalid_length, 0, 0};
        }
        break;
    default: break;
    }

    *bytes_read = static_cast<ULONG>(result.transferred);
    *bytes_needed = static_cast<ULONG>(result.needed);
    return information_status(result.status);
}

// <= DISPATCH_LEVEL. Production Send transfers ownership only through the
// actual adapter-owned path after all hardware/IRQ/recovery admission gates.
extern "C" NDIS_STATUS NTAPI rtl8125_send(NDIS_HANDLE context, PNDIS_PACKET packet, UINT)
{
    auto* adapter = static_cast<Adapter*>(context);
    rtl8125::reactos::AdapterDispatchGuard dispatch(adapter);
    if (!dispatch || InterlockedCompareExchange(&adapter->stop_requested, 0, 0) != 0 ||
        (adapter->interrupts.registered && !rtl8125::reactos::interrupt_registered(*adapter))) {
        return NDIS_STATUS_ADAPTER_NOT_READY;
    }

    if (adapter == nullptr || adapter->phase != AdapterPhase::running || !adapter->link_up ||
        !adapter->hardware.device.initialized || !adapter->hardware.device.running ||
        adapter->hardware.device.recovery_required) { return NDIS_STATUS_ADAPTER_NOT_READY; }
    return rtl8125::reactos::send_packet(adapter->datapath, packet);
}

// DIRQL. M07 recognizes shared events and queues only tracked private DPCs.
extern "C" VOID NTAPI rtl8125_isr(PBOOLEAN recognized, PBOOLEAN queue_wrapper, NDIS_HANDLE context)
{
    rtl8125::reactos::interrupt_isr(static_cast<Adapter*>(context), recognized, queue_wrapper);
}

// The private tracked KDPC calls this actual registered production handler.
extern "C" VOID NTAPI rtl8125_handle_interrupt(NDIS_HANDLE context)
{
    auto* adapter = static_cast<Adapter*>(context);
    if (adapter != nullptr) { rtl8125::reactos::service_interrupt(*adapter); }
}

extern "C" BOOLEAN NTAPI rtl8125_check_for_hang(NDIS_HANDLE context)
{
    auto* adapter = static_cast<Adapter*>(context);
    return adapter != nullptr ? rtl8125::reactos::check_for_hang(*adapter) : FALSE;
}
extern "C" NDIS_STATUS NTAPI rtl8125_reset(PBOOLEAN addressing_reset, NDIS_HANDLE context)
{
    if (addressing_reset != nullptr) { *addressing_reset = FALSE; }
    auto* adapter = static_cast<Adapter*>(context);
    return adapter != nullptr ? rtl8125::reactos::request_reset(*adapter, addressing_reset) : NDIS_STATUS_ADAPTER_NOT_READY;
}

// Stop admission at any IRQL; full asynchronous/DMA retirement belongs to
// the PASSIVE_LEVEL halt/recovery owner, never to a removal notification.
extern "C" VOID NTAPI rtl8125_pnp(NDIS_HANDLE context, NDIS_DEVICE_PNP_EVENT event,
                                 PVOID, ULONG)
{
    auto* adapter = static_cast<Adapter*>(context);
    if (adapter != nullptr && (event == NdisDevicePnPEventRemoved ||
        event == NdisDevicePnPEventSurpriseRemoved || event == NdisDevicePnPEventStopped)) {
        rtl8125::reactos::request_stop(*adapter,
            event == NdisDevicePnPEventRemoved || event == NdisDevicePnPEventSurpriseRemoved);
        rtl8125::reactos::AdapterDispatchGuard dispatch(adapter);
        if (!dispatch) { return; }
        adapter->phase = AdapterPhase::quiescing;
        adapter->link_up = false;
        rtl8125::reactos::stop_datapath(adapter->datapath);
    }
}

extern "C" VOID NTAPI rtl8125_shutdown(PVOID context)
{
    auto* adapter = static_cast<Adapter*>(context);
    if (adapter != nullptr) {
        rtl8125::reactos::shutdown_recovery(*adapter);
        rtl8125::reactos::AdapterDispatchGuard dispatch(adapter);
        if (!dispatch) { return; }
        adapter->phase = AdapterPhase::quiescing;
        adapter->link_up = false;
        rtl8125::reactos::stop_datapath(adapter->datapath);
    }
}

// PASSIVE_LEVEL. The driver-object extension retains wrapper ownership; the
// registered NDIS unload callback terminates it after adapter retirement.
// Every registration failure terminates it locally. No mutable global state.
extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver_object,
                                     PUNICODE_STRING registry_path)
{
    NDIS_HANDLE wrapper{};
    NDIS_MINIPORT_CHARACTERISTICS characteristics{};
    auto& legacy = characteristics.Ndis50Chars.Ndis40Chars.Ndis30Chars;
    legacy.MajorNdisVersion = 5;
    legacy.MinorNdisVersion = 1;
    legacy.InitializeHandler = rtl8125_initialize;
    legacy.HaltHandler = rtl8125_halt;
    legacy.QueryInformationHandler = rtl8125_query;
    legacy.SetInformationHandler = rtl8125_set;
    legacy.SendHandler = rtl8125_send;
    legacy.ISRHandler = rtl8125_isr;
    legacy.HandleInterruptHandler = rtl8125_handle_interrupt;
    legacy.ResetHandler = rtl8125_reset;
    legacy.CheckForHangHandler = rtl8125_check_for_hang;
    characteristics.PnPEventNotifyHandler = rtl8125_pnp;
    characteristics.AdapterShutdownHandler = rtl8125_shutdown;
    NdisMInitializeWrapper(&wrapper, driver_object, registry_path, nullptr);

    if (wrapper == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PVOID registration_storage{};
    const auto extension_status = IoAllocateDriverObjectExtension(
        driver_object, const_cast<char*>(&registration_extension_key),
        sizeof(DriverRegistration), &registration_storage);

    if (!NT_SUCCESS(extension_status)) {
        NdisTerminateWrapper(wrapper, nullptr);
        return extension_status;
    }

    auto* registration = new (registration_storage) DriverRegistration{wrapper, {driver_object, destroy_adapter}};

    const auto status = NdisMRegisterMiniport(wrapper, &characteristics,
                                            sizeof(characteristics));

    if (status != NDIS_STATUS_SUCCESS) {
        registration->wrapper = nullptr;
        NdisTerminateWrapper(wrapper, nullptr);
    } else {
        NdisMRegisterUnloadHandler(wrapper, rtl8125_unload);
    }

    return static_cast<NTSTATUS>(status);
}
