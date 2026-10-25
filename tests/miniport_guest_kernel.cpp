// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.miniport_guest_kernel
Purpose: Run genuine NDIS registration and labelled callback fixtures in ReactOS.
Created: 2026-10-24
Architecture: Opt-in isolated guest driver; imports real NDIS/HAL/kernel APIs.
*/
#include "../src/reactos/adapter.hpp"
#include <new>

extern "C" NTSTATUS NTAPI rtl8125_production_entry(PDRIVER_OBJECT, PUNICODE_STRING);
extern "C" NDIS_STATUS NTAPI rtl8125_initialize(PNDIS_STATUS, PUINT, PNDIS_MEDIUM,
                                               UINT, NDIS_HANDLE, NDIS_HANDLE);
extern "C" VOID NTAPI rtl8125_halt(NDIS_HANDLE);
extern "C" NDIS_STATUS NTAPI rtl8125_query(NDIS_HANDLE, NDIS_OID, PVOID, ULONG,
                                         PULONG, PULONG);
extern "C" NDIS_STATUS NTAPI rtl8125_set(NDIS_HANDLE, NDIS_OID, PVOID, ULONG,
                                       PULONG, PULONG);
extern "C" NDIS_STATUS NTAPI rtl8125_send(NDIS_HANDLE, PNDIS_PACKET, UINT);
extern "C" NDIS_STATUS NTAPI rtl8125_reset(PBOOLEAN, NDIS_HANDLE);
extern "C" VOID NTAPI rtl8125_unload(PDRIVER_OBJECT);

namespace {

struct Checks {
    ULONG total{};
    ULONG failed{};

    void expect(bool condition, PCSTR description) noexcept
    {
        ++total;

        if (!condition) {
            ++failed;
        }

        DbgPrint("RTL8125_M01_GUEST: fixture %s %s\n",
                 condition ? "PASS" : "FAIL", description);
    }
};

void test_rejected_initialization(Checks& checks) noexcept
{
    NDIS_STATUS open_error = NDIS_STATUS_SUCCESS;
    UINT selected = 99;
    NDIS_MEDIUM media[]{NdisMediumWan};
    auto status = rtl8125_initialize(&open_error, &selected, media, 1, nullptr, nullptr);
    checks.expect(status == NDIS_STATUS_UNSUPPORTED_MEDIA &&
                  open_error == NDIS_STATUS_UNSUPPORTED_MEDIA && selected == 0,
                  "unsupported-medium-no-adapter-acquisition");
    status = rtl8125_initialize(&open_error, &selected, nullptr, 1, nullptr, nullptr);
    checks.expect(status == NDIS_STATUS_FAILURE && open_error == NDIS_STATUS_FAILURE,
                  "null-medium-rejected-before-dereference");
    status = rtl8125_initialize(&open_error, &selected, nullptr, 0, nullptr, nullptr);
    checks.expect(status == NDIS_STATUS_UNSUPPORTED_MEDIA,
                  "empty-medium-array-rejected");
    KIRQL previous{};
    KeRaiseIrql(DISPATCH_LEVEL, &previous);
    status = rtl8125_initialize(&open_error, &selected, media, 1, nullptr, nullptr);
    KeLowerIrql(previous);
    checks.expect(status == NDIS_STATUS_FAILURE,
                  "wrong-irql-initialize-rejected-before-allocation");
}

void test_owned_empty_adapter(Checks& checks) noexcept
{
    using rtl8125::reactos::Adapter;

    // Deliberately constructed test state, not an NDIS-initialized adapter.
    // Empty resources own no mapped device, DMA, IRQ, timer or packet. Real
    // NdisAllocateMemoryWithTag/Free execute in kernel; no OS APIs are mocked.
    for (ULONG iteration = 0; iteration < 4; ++iteration) {
        PVOID storage{};
        const auto allocated = NdisAllocateMemoryWithTag(&storage, sizeof(Adapter),
                                                         0x54313252);
        checks.expect(allocated == NDIS_STATUS_SUCCESS && storage != nullptr,
                      "real-ndis-nonpaged-adapter-allocation");

        if (allocated != NDIS_STATUS_SUCCESS || storage == nullptr) {
            continue;
        }

        auto* adapter = new (storage) Adapter{};
        ULONG written = 99;
        ULONG needed = 99;
        ULONG scalar = 99;
        auto status = rtl8125_query(adapter, OID_GEN_LINK_SPEED, &scalar, sizeof(scalar),
                                    &written, &needed);
        checks.expect(status == NDIS_STATUS_SUCCESS && scalar == 0 && written == 4 &&
                      needed == 0, "actual-kernel-oid-disconnected-zero-speed");
        status = rtl8125_query(adapter, OID_802_3_CURRENT_ADDRESS, &scalar, sizeof(scalar),
                                &written, &needed);
        checks.expect(status == NDIS_STATUS_ADAPTER_NOT_READY && written == 0 &&
                      needed == 0, "actual-kernel-oid-refuses-invented-mac");
        scalar = 1;
        status = rtl8125_set(adapter, OID_GEN_CURRENT_PACKET_FILTER, &scalar, sizeof(scalar),
                              &written, &needed);
        checks.expect(status == NDIS_STATUS_NOT_SUPPORTED && written == 0 &&
                      adapter->packet_filter == 0,
                      "actual-kernel-oid-refuses-unimplemented-filter");
        checks.expect(rtl8125_send(adapter, nullptr, 0) == NDIS_STATUS_ADAPTER_NOT_READY,
                      "actual-kernel-send-retains-no-packet");
        BOOLEAN addressing_reset = TRUE;
        checks.expect(rtl8125_reset(&addressing_reset, adapter) ==
                      NDIS_STATUS_NOT_SUPPORTED && addressing_reset == FALSE,
                      "actual-kernel-reset-refuses-false-success");
        rtl8125_halt(adapter);
        DbgPrint("RTL8125_M01_GUEST: fixture PASS empty-owned-adapter-halt-returned iteration %lu\n",
                 iteration);
        ++checks.total;
    }

    rtl8125_halt(nullptr);
    checks.expect(true, "null-halt-returned");
}

} // namespace

// This anonymous guest-only control device makes the harness a legacy service
// for ReactOS IopUnloadDriver. Without a DriverEntry-created device, ReactOS
// clears DRVO_LEGACY_DRIVER and SCM stop refuses even an installed unload
// handler. The production PnP miniport creates no such control device.
extern "C" VOID NTAPI rtl8125_guest_unload(PDRIVER_OBJECT driver_object)
{
    if (driver_object->DeviceObject != nullptr) {
        IoDeleteDevice(driver_object->DeviceObject);
    }

    DbgPrint("RTL8125_M01_GUEST: fixture anonymous-control-device-released\n");
    rtl8125_unload(driver_object);
}

extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver_object,
                                     PUNICODE_STRING registry_path)
{
    DbgPrint("RTL8125_M01_GUEST: begin v2 explicit-wrapper-unload real-ReactOS-kernel no-hardware-device\n");
    const auto status = rtl8125_production_entry(driver_object, registry_path);
    DbgPrint("RTL8125_M01_GUEST: production-NDIS-registration status %08lx\n",
             static_cast<ULONG>(status));

    if (!NT_SUCCESS(status)) {
        return status;
    }

    Checks checks;
    checks.expect(driver_object->DriverUnload == rtl8125_unload,
                  "genuine-ndis-unload-handler-installed");
    test_rejected_initialization(checks);
    test_owned_empty_adapter(checks);
    PDEVICE_OBJECT control_device{};
    const auto control_status = IoCreateDevice(driver_object, 0, nullptr,
                                                FILE_DEVICE_UNKNOWN, 0, FALSE,
                                                &control_device);
    checks.expect(NT_SUCCESS(control_status) && control_device != nullptr,
                  "guest-only-anonymous-control-device-for-scm-unload");

    if (NT_SUCCESS(control_status) && control_device != nullptr) {
        control_device->Flags &= ~DO_DEVICE_INITIALIZING;
        driver_object->DriverUnload = rtl8125_guest_unload;
    }

    DbgPrint("RTL8125_M01_GUEST: end checks %lu failures %lu\n", checks.total, checks.failed);

    // Production registration transferred ownership to NDIS. Returning a test
    // failure here would unload an image with registered callbacks. Tests report
    // failure in the serial log; genuine NDIS wrapper unload retains ownership.
    return STATUS_SUCCESS;
}
