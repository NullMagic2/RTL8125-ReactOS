// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.reactos.hardware_init
Purpose: Adapt checked register operations and NT firmware files to portable code.
Created: 2026-10-24
Architecture: Does not enable hardware or run from current miniport callbacks.
*/
#include "hardware_init.hpp"

namespace rtl8125::reactos {
namespace {
NDIS_STATUS native_status(hardware::Status status) noexcept
{
    return status == hardware::Status::ok ? NDIS_STATUS_SUCCESS :
        status == hardware::Status::invalid_argument ? NDIS_STATUS_INVALID_DATA : NDIS_STATUS_FAILURE;
}

constexpr ULONG firmware_tag = 0x46743552;
}

void release_firmware(FirmwareBlob& blob) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (blob.bytes != nullptr) {
        ExFreePoolWithTag(blob.bytes, firmware_tag);
    }
    blob = {};
}

NDIS_STATUS acquire_firmware(MacVersion version, FirmwareBlob& output) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || output.bytes != nullptr) {
        return NDIS_STATUS_FAILURE;
    }
    const auto* name = firmware_name(version);
    if (name == nullptr) {
        return NDIS_STATUS_NOT_SUPPORTED;
    }
    constexpr WCHAR prefix[] = L"\\SystemRoot\\system32\\drivers\\";
    WCHAR path[128]{};
    ULONG position{};
    for (; position < sizeof(prefix) / sizeof(prefix[0]) - 1; ++position) {
        path[position] = prefix[position];
    }
    for (ULONG index = 0; name[index] != 0; ++index) {
        if (position >= sizeof(path) / sizeof(path[0]) - 1) {
            return NDIS_STATUS_INVALID_DATA;
        }
        path[position++] = name[index] == '/' ? L'\\' : static_cast<WCHAR>(name[index]);
    }
    UNICODE_STRING filename{};
    filename.Buffer = path;
    filename.Length = static_cast<USHORT>(position * sizeof(WCHAR));
    filename.MaximumLength = sizeof(path);
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &filename, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    IO_STATUS_BLOCK io_status{};
    HANDLE file{};
    auto status = ZwCreateFile(&file, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &attributes, &io_status, nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
        FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, nullptr, 0);
    if (!NT_SUCCESS(status)) {
        return NDIS_STATUS_FILE_NOT_FOUND;
    }
    FILE_STANDARD_INFORMATION information{};
    status = ZwQueryInformationFile(file, &io_status, &information, sizeof(information), FileStandardInformation);
    FirmwareBlob blob{};
    NDIS_STATUS result = NDIS_STATUS_INVALID_DATA;
    if (NT_SUCCESS(status) && information.EndOfFile.QuadPart >= 4 &&
        information.EndOfFile.QuadPart <= static_cast<LONGLONG>(hardware::maximum_firmware_size) &&
        !information.Directory) {
        blob.size = static_cast<ULONG>(information.EndOfFile.QuadPart);
        blob.bytes = static_cast<PUCHAR>(ExAllocatePoolWithTag(NonPagedPool, blob.size, firmware_tag));
        if (blob.bytes == nullptr) {
            result = NDIS_STATUS_RESOURCES;
        } else {
            LARGE_INTEGER offset{};
            status = ZwReadFile(file, nullptr, nullptr, nullptr, &io_status, blob.bytes,
                blob.size, &offset, nullptr);
            if (NT_SUCCESS(status) && io_status.Information == blob.size &&
                hardware::validate_firmware(blob.bytes, blob.size, blob.view) == hardware::FirmwareStatus::ok) {
                result = NDIS_STATUS_SUCCESS;
            }
        }
    }
    ZwClose(file);
    if (result != NDIS_STATUS_SUCCESS) {
        release_firmware(blob);
        return result;
    }
    blob.version = version;
    output = blob;
    return result;
}

NDIS_STATUS initialize_hardware(HardwareAccess& access, const FirmwareBlob& firmware,
                                HardwareState& state) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || access.resources == nullptr ||
        access.adapter_handle == nullptr || access.pci_function > 7 ||
        access.resources->chip.status != ChipStatus::supported ||
        firmware.version != access.resources->chip.version || firmware.bytes == nullptr ||
        firmware.view.bytes != firmware.bytes || firmware.size != firmware.view.byte_count) {
        return NDIS_STATUS_INVALID_DATA;
    }
    hardware::Status result{};
    {
        // PHY firmware reset polling and all modelled worst-case waits fit these
        // finite caps; no caller can request an unbounded kernel transaction.
        hardware::RegisterSession io(hardware_register_io(access), {900000, 9000000, 15000000});
        if (hardware::initialize_device(io, firmware.version, firmware.view, state.device)) {
            return NDIS_STATUS_SUCCESS;
        }
        result = io.status();
    }
    if (state.device.recovery_required) {
        hardware::RegisterSession cleanup(hardware_register_io(access), {128, 10000, 100000});
        hardware::contain_device(cleanup, state.device);
    }
    return native_status(result);
}

NDIS_STATUS activate_hardware(HardwareAccess& access, HardwareState& state,
                              const hardware::RingConfiguration& rings) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || access.resources == nullptr ||
        access.resources->chip.version != state.device.version) { return NDIS_STATUS_INVALID_DATA; }
    hardware::Status result{};
    {
        hardware::RegisterSession io(hardware_register_io(access));
        if (hardware::activate_device(io, state.device, rings)) { return NDIS_STATUS_SUCCESS; }
        result = io.status();
    }
    if (state.device.recovery_required) {
        hardware::RegisterSession cleanup(hardware_register_io(access), {128, 10000, 100000});
        hardware::contain_device(cleanup, state.device);
    }
    return native_status(result);
}

NDIS_STATUS query_hardware_link(HardwareAccess& access, const HardwareState& state,
                                hardware::LinkState& output) noexcept
{
    output = {};
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || access.resources == nullptr ||
        access.resources->chip.version != state.device.version) { return NDIS_STATUS_INVALID_DATA; }
    hardware::RegisterSession io(hardware_register_io(access));
    if (!hardware::read_link(io, state.device, output)) {
        output = {};
        return native_status(io.status());
    }
    return NDIS_STATUS_SUCCESS;
}

} // namespace rtl8125::reactos
