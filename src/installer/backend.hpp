/*
Module: rtl8125.installer.backend
Purpose: Own Win32 environment checks, one-device installation and recovery evidence.
Created: 2026-10-24
Architecture: User-mode SetupAPI; preview is separated before any native mutation.
*/
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include "model.hpp"

namespace rtl8125::installer {
struct Device {
    std::wstring instance;
    std::vector<std::wstring> hardware_ids;
    std::wstring current_name;
    std::wstring service;
    Card card{Card::unrelated};
};
struct Snapshot {
    std::vector<Device> devices;
    CompatibilityEvidence evidence;
    Compatibility state{Compatibility::unsupported_os};
    std::wstring issue;
    std::wstring windows;
    std::wstring package;
    std::string ndis_hash;
    bool preview{};
    bool payload_valid{};
};
struct Result {
    bool success{};
    bool reboot{};
    bool preview{};
    DWORD error{};
    std::wstring message;
    std::wstring recovery;
};
using Progress = void (*)(void* context, unsigned step);

// Inspection is read-only. Preparation writes a boot journal; the explicit
// controlled-VM build permits the audited ReactOS ACL stub, without protection.
Snapshot inspect(bool preview);
Result prepare_compatibility(const Snapshot& snapshot);
bool firmware_valid(const std::wstring& path);
std::wstring installed_firmware(const Snapshot& snapshot, bool family_b);
Result install(const Snapshot& snapshot, const Device& selected,
    const std::wstring& firmware_a, const std::wstring& firmware_b,
    Progress progress, void* context);
Result remove(const Snapshot& snapshot, const Device& selected);
Result restore(const Snapshot& snapshot, const Device& selected);
std::wstring error_text(DWORD error);
}
