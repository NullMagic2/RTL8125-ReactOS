/*
Module: rtl8125.installer.backend
Purpose: Install only a selected real RTL8125 with reboot-proven NDIS compatibility.
Created: 2026-10-24
Architecture: Actual NT5 SetupAPI and protected recovery journals; no OS binary updater.
*/
#include "backend.hpp"
#include "installer_build.hpp"
#include "package_contract.hpp"
#include "../hardware/firmware.hpp"
#include "../core/chip.hpp"
#include <cfgmgr32.h>
#include <sddl.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstring>
#if !defined(_M_IX86) && !defined(__i386__)
#error The native installer backend requires an actual i386 compiler target
#endif

namespace rtl8125::installer {
namespace {
constexpr wchar_t journal_key[] = L"Software\\RTL8125ReactOS\\Installer";
constexpr wchar_t protected_acl[] = L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)";
constexpr wchar_t registry_acl[] = L"D:P(A;CI;KA;;;SY)(A;CI;KA;;;BA)";
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE input = INVALID_HANDLE_VALUE) : value(input) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) { CloseHandle(value); } }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct DeviceSet {
    HDEVINFO value{INVALID_HANDLE_VALUE};
    ~DeviceSet() { if (value != INVALID_HANDLE_VALUE) { SetupDiDestroyDeviceInfoList(value); } }
};
struct Key {
    HKEY value{};
    ~Key() { if (value != nullptr) { RegCloseKey(value); } }
};
bool equal(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
std::wstring wide(const std::string& input) { return std::wstring(input.begin(), input.end()); }
bool is_wine() { return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr; }
std::wstring join(const std::wstring& root, const wchar_t* name) { return root + L"\\" + name; }

bool read_file(const std::wstring& path, std::vector<std::uint8_t>& bytes, DWORD maximum)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER length{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &length) ||
        length.QuadPart < 0 || length.QuadPart > maximum) { return false; }
    bytes.resize(static_cast<std::size_t>(length.QuadPart));
    DWORD read{};
    return ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
}

bool file_hash(const std::wstring& path, std::string& output, std::uint64_t* write_time = nullptr)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER length{};
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &length) ||
        length.QuadPart <= 0 || length.QuadPart > 32 * 1024 * 1024 ||
        !GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) { return false; }
    Sha256 hash;
    std::uint8_t buffer[16384];
    std::uint64_t total{};
    DWORD read{};
    for (;;) {
        if (!ReadFile(file.value, buffer, sizeof(buffer), &read, nullptr)) { return false; }
        if (read == 0) { break; }
        total += read;
        if (total > static_cast<std::uint64_t>(length.QuadPart)) { return false; }
        hash.update(buffer, read);
    }
    if (total != static_cast<std::uint64_t>(length.QuadPart)) { return false; }
    output = hash.finish();
    if (write_time != nullptr) {
        *write_time = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    }
    return true;
}

std::wstring property(HDEVINFO set, SP_DEVINFO_DATA& device, DWORD code)
{
    wchar_t buffer[4096]{};
    DWORD type{}, size{};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, code, &type,
            reinterpret_cast<PBYTE>(buffer), sizeof(buffer), &size) ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t) || size > sizeof(buffer) || size % 2 != 0 ||
        buffer[size / sizeof(wchar_t) - 1] != 0) { return {}; }
    return buffer;
}

std::vector<std::wstring> hardware_ids(HDEVINFO set, SP_DEVINFO_DATA& device)
{
    wchar_t buffer[8192]{};
    DWORD type{}, size{};
    std::vector<std::wstring> result;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, &type,
            reinterpret_cast<PBYTE>(buffer), sizeof(buffer), &size) || type != REG_MULTI_SZ ||
        size < 4 || size > sizeof(buffer) || size % 2 != 0) { return result; }
    const auto length = size / sizeof(wchar_t);
    if (buffer[length - 1] != 0 || buffer[length - 2] != 0) { return result; }
    for (std::size_t position = 0; position < length && buffer[position] != 0;) {
        const auto* end = std::find(buffer + position, buffer + length, L'\0');
        if (end == buffer + length) { return {}; }
        result.emplace_back(buffer + position, static_cast<std::size_t>(end-buffer-position));
        position = static_cast<std::size_t>(end - buffer) + 1;
    }
    return result;
}

bool open_selected(DeviceSet& set, SP_DEVINFO_DATA& info, const Device& selected)
{
    set.value = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    info.cbSize = sizeof(info);
    if (set.value == INVALID_HANDLE_VALUE || !SetupDiOpenDeviceInfoW(set.value,
            selected.instance.c_str(), nullptr, 0, &info)) { return false; }
    return identify(hardware_ids(set.value, info)) == selected.card && selected.card != Card::unrelated;
}

bool administrator()
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID sid{};
    BOOL member{};
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &sid)) { return false; }
    const auto checked = CheckTokenMembership(nullptr, sid, &member);
    FreeSid(sid);
    return checked && member;
}

bool native_reactos()
{
    if (is_wine()) { return false; }
    SYSTEM_INFO info{};
    const auto native_info = reinterpret_cast<void (WINAPI*)(LPSYSTEM_INFO)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetNativeSystemInfo"));
    if (native_info != nullptr) { native_info(&info); } else { GetSystemInfo(&info); }
    if (info.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_INTEL) { return false; }
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_QUERY_VALUE, &key.value) != ERROR_SUCCESS) { return false; }
    wchar_t product[256]{};
    DWORD type{}, bytes = sizeof(product);
    return RegQueryValueExW(key.value, L"ProductName", nullptr, &type,
        reinterpret_cast<BYTE*>(product), &bytes) == ERROR_SUCCESS && type == REG_SZ &&
        bytes >= 2 && bytes <= sizeof(product) && bytes % 2 == 0 && product[bytes / 2 - 1] == 0 &&
        std::wstring(product).find(L"ReactOS") != std::wstring::npos;
}

// Layouts follow the matched ReactOS RTL_PROCESS_MODULE_INFORMATION and the first
// eight bytes of SYSTEM_TIMEOFDAY_INFORMATION. They are read only after length checks.
struct ModuleEntry {
    PVOID section, mapped_base, image_base;
    ULONG image_size, flags;
    USHORT load_order, init_order, load_count, filename_offset;
    unsigned char path[256];
};
static_assert(sizeof(ModuleEntry)==284,"Matched i386 NT module record layout");
bool boot_ndis_service(const std::wstring& windows, std::uint64_t boot_time)
{
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\NDIS",
            0, KEY_QUERY_VALUE, &key.value) != ERROR_SUCCESS) { return false; }
    DWORD start{}, service_type{}, type{}, bytes = sizeof(start);
    if (RegQueryValueExW(key.value, L"Start", nullptr, &type,
            reinterpret_cast<BYTE*>(&start), &bytes) != ERROR_SUCCESS ||
        type != REG_DWORD || bytes != sizeof(start)) { return false; }
    bytes = sizeof(service_type);
    if (RegQueryValueExW(key.value, L"Type", nullptr, &type,
            reinterpret_cast<BYTE*>(&service_type), &bytes) != ERROR_SUCCESS ||
        type != REG_DWORD || bytes != sizeof(service_type)) { return false; }
    wchar_t image[1024]{};
    bytes = sizeof(image);
    if (RegQueryValueExW(key.value, L"ImagePath", nullptr, &type,
            reinterpret_cast<BYTE*>(image), &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < 2 ||
        bytes > sizeof(image) || bytes % 2 != 0 || image[bytes / 2 - 1] != 0) { return false; }
    FILETIME write{};
    if (RegQueryInfoKeyW(key.value, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, &write) != ERROR_SUCCESS) { return false; }
    const auto stamp = (static_cast<std::uint64_t>(write.dwHighDateTime) << 32) | write.dwLowDateTime;
    return boot_ndis_identity(std::wstring(image, bytes / 2 - 1), windows,
        start, service_type, stamp, boot_time);
}

bool loaded_ndis(const std::wstring& windows, std::uint64_t& boot_time)
{
    using Query = LONG (NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (query == nullptr) { return false; }
    ULONG returned{};
    LARGE_INTEGER boot{};
    if (query(3, &boot, sizeof(boot), &returned) < 0 || returned != sizeof(boot) || boot.QuadPart <= 0) { return false; }
    boot_time = static_cast<std::uint64_t>(boot.QuadPart);
    std::vector<BYTE> modules(1024 * 1024);
    if (query(11, modules.data(), static_cast<ULONG>(modules.size()), &returned) < 0 ||
        returned < sizeof(ULONG) || returned > modules.size()) { return false; }
    ULONG count{};
    std::memcpy(&count, modules.data(), sizeof(count));
    if (count > (returned - sizeof(ULONG)) / sizeof(ModuleEntry)) { return false; }
    unsigned matches{};
    bool resolved{};
    for (ULONG index = 0; index < count; ++index) {
        ModuleEntry entry{};
        std::memcpy(&entry, modules.data() + sizeof(ULONG) + index * sizeof(entry), sizeof(entry));
        const auto end = std::find(entry.path, entry.path + sizeof(entry.path), 0);
        if (end == entry.path + sizeof(entry.path) || entry.filename_offset >= end - entry.path) { continue; }
        const std::string name(reinterpret_cast<char*>(entry.path + entry.filename_offset));
        if (_stricmp(name.c_str(), "ndis.sys") != 0) { continue; }
        ++matches;
        const std::wstring path(entry.path, end);
        resolved = equal(path, L"\\SystemRoot\\system32\\drivers\\ndis.sys") ||
            equal(path, join(windows, L"system32\\drivers\\ndis.sys")) ||
            equal(path, L"\\??\\" + join(windows, L"system32\\drivers\\ndis.sys")) ||
            (equal(path, L"ndis.sys") && boot_ndis_service(windows, boot_time));
    }
    return matches == 1 && resolved;
}

bool journal_usable(HKEY key)
{
    std::vector<BYTE> descriptor(4096);
    DWORD bytes = static_cast<DWORD>(descriptor.size());
    if (RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, descriptor.data(), &bytes) != ERROR_SUCCESS) { return false; }
    BOOL present{}, defaulted{};
    PACL acl{};
    if (!GetSecurityDescriptorDacl(descriptor.data(), &present, &acl, &defaulted) || !present || acl == nullptr) { return false; }
#if defined(RTL8125_TRUSTED_REACTOS_GUEST)
    // The audited ReactOS CM security methods synthesize one World/GENERIC_ALL
    // ACE and ignore security updates. Only an explicitly built controlled-VM
    // variant accepts this shape; the normal installer keeps its ACL requirement.
    if (acl->AceCount == 1 && native_reactos() && administrator()) {
        PVOID pointer{};
        if (GetAce(acl, 0, &pointer)) {
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(pointer);
            if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Mask == GENERIC_ALL &&
                IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), WinWorldSid)) { return true; }
        }
    }
#endif
    if (acl->AceCount != 2) { return false; }
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        PVOID pointer{};
        if (!GetAce(acl, index, &pointer)) { return false; }
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(pointer);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
            (!IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), WinLocalSystemSid) &&
             !IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), WinBuiltinAdministratorsSid))) { return false; }
    }
    return true;
}

void read_observation(CompatibilityEvidence& evidence, const std::string& hash)
{
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, journal_key, 0, KEY_QUERY_VALUE | READ_CONTROL, &key.value) != ERROR_SUCCESS ||
        !journal_usable(key.value)) { return; }
    wchar_t previous[65]{};
    DWORD type{}, bytes = sizeof(previous);
    if (RegQueryValueExW(key.value, L"ObservedNdisHash", nullptr, &type,
            reinterpret_cast<BYTE*>(previous), &bytes) != ERROR_SUCCESS || type != REG_SZ ||
        bytes != sizeof(previous) || previous[64] != 0 || wide(hash) != previous) { return; }
    bytes = sizeof(evidence.observed_boot_time);
    if (RegQueryValueExW(key.value, L"ObservedBootTime", nullptr, &type,
            reinterpret_cast<BYTE*>(&evidence.observed_boot_time), &bytes) == ERROR_SUCCESS &&
        type == REG_QWORD && bytes == sizeof(evidence.observed_boot_time)) { evidence.journal_hash_matches = true; }
}

bool payload(const std::wstring& root)
{
    std::string sys_hash, inf_hash;
    std::vector<std::uint8_t> sys, inf;
    return file_hash(join(root, L"rtl8125.sys"), sys_hash) && sys_hash == RTL8125_SETUP_DRIVER_HASH &&
        file_hash(join(root, L"rtl8125.inf"), inf_hash) && inf_hash == RTL8125_SETUP_INF_HASH &&
        read_file(join(root, L"rtl8125.sys"), sys, 16 * 1024 * 1024) &&
        read_file(join(root, L"rtl8125.inf"), inf, 65536) &&
        package::PeView(sys).valid() && package::operational_variant(sys) &&
        package::validate_inf(std::string(inf.begin(), inf.end()));
}

Result failure(const wchar_t* message, DWORD error = GetLastError())
{
    return {false, false, false, error, std::wstring(message) + L"\n" + error_text(error), {}};
}
}

std::wstring error_text(DWORD error)
{
    wchar_t buffer[768]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
        0, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(wchar_t)), nullptr);
    return L"Error " + std::to_wstring(error) + (buffer[0] != 0 ? L": " + std::wstring(buffer) : L"");
}

Snapshot inspect(bool preview)
{
    Snapshot result;
    result.preview = preview;
    wchar_t path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length != 0 && length < MAX_PATH) {
        result.package = path;
        const auto separator = result.package.find_last_of(L"\\/");
        result.package = separator == std::wstring::npos ? std::wstring{} : result.package.substr(0,separator);
    }
    if (preview) {
        result.devices.push_back({L"Preview device", {L"PCI\\VEN_10EC&DEV_8125&SUBSYS_7E271462&REV_05"},
            L"RTL8125B", L"", Card::rtl8125b});
        result.evidence = {true,true,true,true,true,200,100,50};
        result.state = Compatibility::ready;
        result.payload_valid = true;
        return result;
    }
    const auto count = GetWindowsDirectoryW(path, MAX_PATH);
    if (count != 0 && count < MAX_PATH) { result.windows = path; }
    result.evidence.reactos_i386 = native_reactos();
    result.evidence.administrator = administrator();
    if (result.evidence.reactos_i386 && result.evidence.administrator) {
        result.evidence.loaded_path_matches = loaded_ndis(result.windows, result.evidence.boot_time);
        result.evidence.approved_hash = file_hash(join(result.windows, L"system32\\drivers\\ndis.sys"),
            result.ndis_hash, &result.evidence.file_write_time) && result.ndis_hash == RTL8125_SETUP_NDIS_HASH;
        read_observation(result.evidence, result.ndis_hash);
    }
    result.state = compatibility(result.evidence);
    switch (result.state) {
    case Compatibility::unsupported_os: result.issue = L"Open this installer inside ReactOS i386. Wine preview cannot install drivers."; break;
    case Compatibility::administrator_required: result.issue = L"Sign in as an administrator, then reopen setup."; break;
    case Compatibility::wrong_image: result.issue = L"Use a ReactOS image with the matching patched NDIS driver. Read REACTOS_COMPATIBILITY.txt in this package. Setup does not replace core OS files."; break;
    case Compatibility::reboot_required: result.issue = L"Confirm this compatible image, restart ReactOS, then reopen setup. This checks that the approved network core was present before this boot."; break;
    case Compatibility::ready: break;
    }
    result.payload_valid = payload(result.package);
    DeviceSet set;
    set.value = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set.value != INVALID_HANDLE_VALUE) {
        for (DWORD index = 0; index < 4096; ++index) {
            SP_DEVINFO_DATA info{};
            info.cbSize = sizeof(info);
            if (!SetupDiEnumDeviceInfo(set.value, index, &info)) { break; }
            auto ids = hardware_ids(set.value, info);
            const auto card = identify(ids);
            if (card == Card::unrelated) { continue; }
            wchar_t instance[4096]{};
            if (!SetupDiGetDeviceInstanceIdW(set.value, &info, instance, 4096, nullptr)) { continue; }
            auto name = property(set.value, info, SPDRP_FRIENDLYNAME);
            if (name.empty()) { name = property(set.value, info, SPDRP_DEVICEDESC); }
            result.devices.push_back({instance, std::move(ids), std::move(name), property(set.value, info, SPDRP_SERVICE), card});
        }
    }
    return result;
}

Result prepare_compatibility(const Snapshot& snapshot)
{
    if (snapshot.preview || is_wine()) { return failure(L"Preview cannot write a compatibility journal.", ERROR_ACCESS_DENIED); }
    auto fresh = inspect(false);
    if (!fresh.evidence.reactos_i386 || !fresh.evidence.administrator || !fresh.evidence.approved_hash ||
        !fresh.evidence.loaded_path_matches || fresh.evidence.boot_time == 0) { return failure(L"A matching patched ReactOS image is required.", ERROR_REVISION_MISMATCH); }
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(registry_acl, SDDL_REVISION_1, &descriptor, nullptr)) { return failure(L"Cannot protect compatibility evidence."); }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    Key key;
    auto status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, journal_key, 0, nullptr, 0,
        KEY_SET_VALUE | READ_CONTROL | WRITE_DAC, &attributes, &key.value, nullptr);
    if (status == ERROR_SUCCESS) { status = RegSetKeySecurity(key.value, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor); }
    LocalFree(descriptor);
    if (status != ERROR_SUCCESS) { return failure(L"Cannot protect the machine journal.", status); }
    if (!journal_usable(key.value)) { return failure(L"This ReactOS build cannot protect the setup journal. Use the explicitly built controlled-VM variant for a trusted test guest.", ERROR_NOT_SUPPORTED); }
    const auto hash = wide(fresh.ndis_hash);
    status = RegSetValueExW(key.value, L"ObservedNdisHash", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(hash.c_str()), static_cast<DWORD>((hash.size() + 1) * 2));
    if (status == ERROR_SUCCESS) { status = RegSetValueExW(key.value, L"ObservedBootTime", 0, REG_QWORD,
        reinterpret_cast<const BYTE*>(&fresh.evidence.boot_time), sizeof(fresh.evidence.boot_time)); }
    if (status == ERROR_SUCCESS) { status = RegFlushKey(key.value); }
    if (status != ERROR_SUCCESS) { return failure(L"Cannot save reboot evidence.", status); }
    return {true,true,false,0,L"Compatible image recorded. Restart ReactOS and reopen setup to verify the new boot.",{}};
}

std::wstring installed_firmware(const Snapshot& snapshot, bool family_b)
{
    auto name=wide(firmware_name(family_b?MacVersion::rtl8125b:MacVersion::rtl8125a));
    std::replace(name.begin(),name.end(),L'/',L'\\');
    return join(join(snapshot.windows,L"system32\\drivers"),name.c_str());
}

bool firmware_valid(const std::wstring& path)
{
    std::vector<std::uint8_t> bytes;
    hardware::FirmwareView view;
    return read_file(path, bytes, static_cast<DWORD>(hardware::maximum_firmware_size)) &&
        hardware::validate_firmware(bytes.data(), bytes.size(), view) == hardware::FirmwareStatus::ok;
}

namespace {
std::wstring device_key(const Device& device)
{
    auto instance = device.instance;
    std::transform(instance.begin(), instance.end(), instance.begin(), [](wchar_t value) { return std::towupper(value); });
    Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(instance.data()), instance.size() * sizeof(wchar_t));
    return std::wstring(journal_key) + L"\\Devices\\" + wide(hash.finish());
}

bool protected_directory(const std::wstring& path)
{
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(protected_acl, SDDL_REVISION_1, &descriptor, nullptr)) { return false; }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const auto created = CreateDirectoryW(path.c_str(), &attributes);
    const auto error = GetLastError();
    const auto flags = GetFileAttributesW(path.c_str());
    bool result = (created || error == ERROR_ALREADY_EXISTS) && flags != INVALID_FILE_ATTRIBUTES &&
        (flags & FILE_ATTRIBUTE_DIRECTORY) != 0 && (flags & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    if (result && !SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor)) {
        DWORD filesystem_flags{};
        const auto volume = path.size() >= 3 && path[1] == L':' ? path.substr(0,3) : std::wstring{};
        // FAT cannot persist directory ACLs. Backup integrity is separately
        // anchored in the protected machine registry before any mutation.
        result = !volume.empty() && GetVolumeInformationW(volume.c_str(),nullptr,0,nullptr,nullptr,
            &filesystem_flags,nullptr,0) && (filesystem_flags & FILE_PERSISTENT_ACLS) == 0;
    }
    LocalFree(descriptor);
    return result;
}

bool write_text(const std::wstring& file, const std::wstring& text)
{
    Handle handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) { return false; }
    const wchar_t bom = 0xfeff;
    DWORD wrote{};
    return WriteFile(handle.value, &bom, sizeof(bom), &wrote, nullptr) && wrote == sizeof(bom) &&
        WriteFile(handle.value, text.data(), static_cast<DWORD>(text.size() * 2), &wrote, nullptr) &&
        wrote == text.size() * 2 && FlushFileBuffers(handle.value);
}

bool backup_file(const std::wstring& original, const std::wstring& backup)
{
    const auto flags = GetFileAttributesW(original.c_str());
    const auto error=GetLastError();
    if (flags == INVALID_FILE_ATTRIBUTES && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) { return true; }
    return flags != INVALID_FILE_ATTRIBUTES && (flags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        CopyFileW(original.c_str(), backup.c_str(), TRUE);
}

std::wstring previous_inf(HDEVINFO set, SP_DEVINFO_DATA& device, const Snapshot& snapshot)
{
    Key key;
    key.value = SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
    if (key.value == INVALID_HANDLE_VALUE) { key.value = nullptr; return {}; }
    wchar_t name[260]{};
    DWORD type{}, bytes = sizeof(name);
    if (RegQueryValueExW(key.value, L"InfPath", nullptr, &type, reinterpret_cast<BYTE*>(name), &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < 2 || bytes > sizeof(name) || bytes % 2 != 0 || name[bytes / 2 - 1] != 0 || !safe_component(name)) { return {}; }
    return join(join(snapshot.windows, L"inf"), name);
}

std::wstring registry_string(HKEY key, const wchar_t* name)
{
    wchar_t value[4096]{};
    DWORD type{}, bytes=sizeof(value);
    return RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes) == ERROR_SUCCESS &&
        type == REG_SZ && bytes >= 2 && bytes <= sizeof(value) && bytes % 2 == 0 && value[bytes/2-1] == 0 ? value : std::wstring{};
}

bool set_string(HKEY key, const wchar_t* name, const std::wstring& value)
{
    return RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size()+1)*2)) == ERROR_SUCCESS;
}

std::wstring previous_binary(const Snapshot& snapshot, const std::wstring& service)
{
    if (!safe_component(service)) { return {}; }
    Key key;
    const auto name = L"SYSTEM\\CurrentControlSet\\Services\\" + service;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,name.c_str(),0,KEY_QUERY_VALUE,&key.value) != ERROR_SUCCESS) { return {}; }
    wchar_t value[1024]{};
    DWORD type{}, bytes=sizeof(value);
    if (RegQueryValueExW(key.value,L"ImagePath",nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < 2 || bytes > sizeof(value) || bytes % 2 != 0 || value[bytes/2-1] != 0) { return {}; }
    const std::wstring path=value;
    for (const auto& prefix : {std::wstring(L"\\SystemRoot\\system32\\drivers\\"),std::wstring(L"%SystemRoot%\\system32\\drivers\\"),
            join(snapshot.windows,L"system32\\drivers\\"),std::wstring(L"system32\\drivers\\")}) {
        if (path.size() > prefix.size() && equal(path.substr(0,prefix.size()),prefix) && safe_component(path.substr(prefix.size()))) {
            return path.substr(prefix.size());
        }
    }
    return {};
}

bool save_transaction(const Device& selected, const std::wstring& path, const Snapshot& snapshot,
    const std::wstring& old_inf, const std::wstring& old_service)
{
    Key key;
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(registry_acl,SDDL_REVISION_1,&descriptor,nullptr)) { return false; }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    auto status=RegCreateKeyExW(HKEY_LOCAL_MACHINE, device_key(selected).c_str(), 0, nullptr, 0,
        KEY_SET_VALUE|KEY_QUERY_VALUE|READ_CONTROL|WRITE_DAC, &attributes, &key.value, nullptr);
    if (status == ERROR_SUCCESS) { status=RegSetKeySecurity(key.value,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,descriptor); }
    LocalFree(descriptor);
    if (status != ERROR_SUCCESS || !journal_usable(key.value)) { return false; }
    if (!set_string(key.value,L"LatestAttempt",path)) { return false; }
    if (!registry_string(key.value,L"Transaction").empty()) {
        return equal(registry_string(key.value,L"Instance"),selected.instance) && RegFlushKey(key.value) == ERROR_SUCCESS;
    }
    const auto binary=previous_binary(snapshot,old_service);
    std::string binary_hash;
    if (!binary.empty() && !file_hash(join(join(snapshot.windows,L"system32\\drivers"),binary.c_str()),binary_hash)) { return false; }
    if (!set_string(key.value,L"Instance",selected.instance) || !set_string(key.value,L"PreviousInfPath",old_inf) ||
        !set_string(key.value,L"PreviousService",old_service) || !set_string(key.value,L"PreviousBinary",binary) ||
        !set_string(key.value,L"PreviousBinaryHash",wide(binary_hash)) || !set_string(key.value,L"PreviousName",selected.current_name)) { return false; }
    for (const auto* file : {L"previous.inf",L"rtl8125.sys.before",L"rtl8125a-3.fw.before",L"rtl8125b-2.fw.before"}) {
        std::string hash;
        const auto backup=join(path,file);
        const auto flags=GetFileAttributesW(backup.c_str());
        const auto attribute_error=GetLastError();
        if (flags != INVALID_FILE_ATTRIBUTES && !file_hash(backup,hash)) { return false; }
        if (flags == INVALID_FILE_ATTRIBUTES && attribute_error != ERROR_FILE_NOT_FOUND) { return false; }
        if (!set_string(key.value,file,wide(hash))) { return false; }
    }
    return set_string(key.value,L"Transaction",path) && RegFlushKey(key.value) == ERROR_SUCCESS;
}

std::wstring transaction_for(const Snapshot& snapshot, const Device& selected)
{
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, device_key(selected).c_str(), 0, KEY_QUERY_VALUE|READ_CONTROL, &key.value) != ERROR_SUCCESS || !journal_usable(key.value)) { return {}; }
    wchar_t instance[4096]{}, path[MAX_PATH]{};
    DWORD type{}, bytes = sizeof(instance);
    if (RegQueryValueExW(key.value, L"Instance", nullptr, &type, reinterpret_cast<BYTE*>(instance), &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < 2 || bytes > sizeof(instance) || bytes % 2 != 0 || instance[bytes / 2 - 1] != 0 || !equal(instance, selected.instance)) { return {}; }
    bytes = sizeof(path);
    if (RegQueryValueExW(key.value, L"Transaction", nullptr, &type, reinterpret_cast<BYTE*>(path), &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < 2 || bytes > sizeof(path) || bytes % 2 != 0 || path[bytes / 2 - 1] != 0) { return {}; }
    const auto prefix = join(snapshot.windows, L"RTL8125-Setup\\");
    const std::wstring result = path;
    if (result.size() <= prefix.size() || !equal(result.substr(0, prefix.size()), prefix) ||
        !safe_component(result.substr(prefix.size()))) { return {}; }
    const auto flags = GetFileAttributesW(result.c_str());
    const auto parent_flags=GetFileAttributesW(join(snapshot.windows,L"RTL8125-Setup").c_str());
    return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (flags & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && parent_flags != INVALID_FILE_ATTRIBUTES &&
        (parent_flags & FILE_ATTRIBUTE_DIRECTORY) != 0 && (parent_flags & FILE_ATTRIBUTE_REPARSE_POINT) == 0 ? result : std::wstring{};
}

bool select_inf(HDEVINFO set, SP_DEVINFO_DATA& device, const std::wstring& path, const wchar_t* expected_name, bool no_copy=false)
{
    if (path.size() >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
    SP_DEVINSTALL_PARAMS_W parameters{};
    parameters.cbSize = sizeof(parameters);
    if (!SetupDiGetDeviceInstallParamsW(set, &device, &parameters)) { return false; }
    parameters.Flags |= DI_ENUMSINGLEINF;
    if (no_copy) { parameters.Flags |= DI_NOFILECOPY; }
    std::copy(path.begin(), path.end(), parameters.DriverPath);
    parameters.DriverPath[path.size()] = 0;
    if (!SetupDiSetDeviceInstallParamsW(set, &device, &parameters) ||
        !SetupDiBuildDriverInfoList(set, &device, SPDIT_COMPATDRIVER) ||
        !SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, set, &device)) { return false; }
    SP_DRVINFO_DATA_W driver{};
    driver.cbSize = sizeof(driver);
    if (!SetupDiGetSelectedDriverW(set, &device, &driver)) { return false; }
    if (expected_name != nullptr && !equal(driver.Description, expected_name)) { SetLastError(ERROR_NO_COMPAT_DRIVERS); return false; }
    return true;
}

bool install_selected(HDEVINFO set, SP_DEVINFO_DATA& device)
{
    // Match the actual newdev installation sequence, restricted to this one
    // already-opened instance and its single compatible INF source.
    for (const DI_FUNCTION function : {DIF_ALLOW_INSTALL, DIF_NEWDEVICEWIZARD_PREANALYZE,
            DIF_NEWDEVICEWIZARD_POSTANALYZE, DIF_INSTALLDEVICEFILES, DIF_REGISTER_COINSTALLERS,
            DIF_INSTALLINTERFACES, DIF_INSTALLDEVICE, DIF_NEWDEVICEWIZARD_FINISHINSTALL}) {
        if (!SetupDiCallClassInstaller(function, set, &device)) { return false; }
    }
    return true;
}

bool disable(HDEVINFO set, SP_DEVINFO_DATA& device)
{
    SP_PROPCHANGE_PARAMS parameters{};
    parameters.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    parameters.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    parameters.StateChange = DICS_DISABLE;
    parameters.Scope = DICS_FLAG_GLOBAL;
    return SetupDiSetClassInstallParamsW(set, &device, &parameters.ClassInstallHeader, sizeof(parameters)) &&
        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device);
}

bool enable(HDEVINFO set, SP_DEVINFO_DATA& device)
{
    SP_PROPCHANGE_PARAMS parameters{};
    parameters.ClassInstallHeader.cbSize=sizeof(SP_CLASSINSTALL_HEADER);
    parameters.ClassInstallHeader.InstallFunction=DIF_PROPERTYCHANGE;
    parameters.StateChange=DICS_ENABLE;
    parameters.Scope=DICS_FLAG_GLOBAL;
    return SetupDiSetClassInstallParamsW(set,&device,&parameters.ClassInstallHeader,sizeof(parameters)) &&
        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE,set,&device);
}

Result with_recovery(Result result, const std::wstring& path)
{
    result.recovery = path;
    if (!path.empty()) {
        write_text(join(path, result.success ? L"complete.txt" : L"failure.txt"), result.message + L"\r\n" + error_text(result.error));
    }
    return result;
}
}

Result install(const Snapshot& snapshot, const Device& selected, const std::wstring& firmware_a,
    const std::wstring& firmware_b, Progress progress, void* context)
{
    if (snapshot.preview) {
        if (progress != nullptr) { for (unsigned step = 1; step <= 4; ++step) { progress(context, step); Sleep(400); } }
        return {true,false,true,0,L"Preview complete. No driver, firmware or registry settings were changed.",{}};
    }
    if (is_wine()) { return failure(L"Installation is unavailable in Wine.", ERROR_ACCESS_DENIED); }
    if (progress != nullptr) { progress(context, 1); }
    const auto fresh = inspect(false);
    if (fresh.state != Compatibility::ready || !fresh.payload_valid || fresh.package != snapshot.package) {
        return failure(L"System or package verification changed. Reopen setup and check requirements.", ERROR_REVISION_MISMATCH);
    }
    for (const auto& device : fresh.devices) {
        if (!equal(device.instance,selected.instance) && equal(device.service,L"Rtl8125")) {
            return failure(L"Another adapter uses this driver. Keep its shared files unchanged and install in a separate test image.",ERROR_BUSY);
        }
    }
    // Keep these read handles open through installation. FILE_SHARE_READ blocks
    // writes, replacement and deletion after the final identity check.
    Handle ndis(CreateFileW(join(fresh.windows,L"system32\\drivers\\ndis.sys").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    Handle driver(CreateFileW(join(fresh.package,L"rtl8125.sys").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    Handle inf(CreateFileW(join(fresh.package,L"rtl8125.inf").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (ndis.value == INVALID_HANDLE_VALUE || driver.value == INVALID_HANDLE_VALUE || inf.value == INVALID_HANDLE_VALUE ||
        !payload(fresh.package)) { return failure(L"Cannot lock the verified driver package."); }
    std::string hash;
    std::uint64_t write_time{};
    if (!file_hash(join(fresh.windows,L"system32\\drivers\\ndis.sys"),hash,&write_time) ||
        hash != RTL8125_SETUP_NDIS_HASH || write_time != fresh.evidence.file_write_time) { return failure(L"The network core changed during verification.",ERROR_REVISION_MISMATCH); }
    DeviceSet set;
    SP_DEVINFO_DATA info{};
    if (!open_selected(set, info, selected)) { return failure(L"The selected network card is no longer available.", ERROR_NO_SUCH_DEVINST); }
    const bool need_a = selected.card != Card::rtl8125b;
    const auto source_a = firmware_a.empty() ? installed_firmware(fresh,false) : firmware_a;
    const auto source_b = firmware_b.empty() ? installed_firmware(fresh,true) : firmware_b;
    const auto basename=[](const std::wstring& path){const auto index=path.find_last_of(L"\\/");return path.substr(index==std::wstring::npos?0:index+1);};
    if (!equal(basename(source_b),basename(installed_firmware(fresh,true))) ||
        (need_a&&!equal(basename(source_a),basename(installed_firmware(fresh,false))))) {
        return failure(L"Select firmware with its original RTL8125A/B filename; do not rename compressed or different-family files.",ERROR_INVALID_DATA);
    }
    Handle fw_b(CreateFileW(source_b.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    Handle fw_a(need_a ? CreateFileW(source_a.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr) : INVALID_HANDLE_VALUE);
    if (fw_b.value == INVALID_HANDLE_VALUE || !firmware_valid(source_b) ||
        (need_a && (fw_a.value == INVALID_HANDLE_VALUE || !firmware_valid(source_a)))) {
        return failure(L"Select valid uncompressed Realtek firmware before installation.", ERROR_INVALID_DATA);
    }
    const auto root = join(fresh.windows,L"RTL8125-Setup");
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t name[80]{};
    std::swprintf(name,80,L"%04u%02u%02u-%02u%02u%02u-%lu",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetTickCount());
    const auto transaction = join(root,name);
    if (!protected_directory(root) || !CreateDirectoryW(transaction.c_str(),nullptr)) { return failure(L"Cannot create a protected recovery folder."); }
    if (!write_text(join(transaction,L"started.txt"),L"RTL8125 setup\r\nDevice: " + selected.instance +
            L"\r\nName: " + card_name(selected.card) + L"\r\nNDIS SHA256: " + wide(hash) + L"\r\nBoot: " + std::to_wstring(fresh.evidence.boot_time))) {
        return with_recovery(failure(L"Cannot save recovery evidence."),transaction);
    }
    if (progress != nullptr) { progress(context,2); }
    const auto old_inf = previous_inf(set.value,info,fresh);
    if ((!old_inf.empty() && !CopyFileW(old_inf.c_str(),join(transaction,L"previous.inf").c_str(),TRUE)) ||
        !backup_file(join(fresh.windows,L"system32\\drivers\\rtl8125.sys"),join(transaction,L"rtl8125.sys.before")) ||
        !backup_file(installed_firmware(fresh,true),join(transaction,L"rtl8125b-2.fw.before")) ||
        (need_a && !backup_file(installed_firmware(fresh,false),join(transaction,L"rtl8125a-3.fw.before")))) {
        return with_recovery(failure(L"Cannot preserve previous driver files. No driver binding was changed."),transaction);
    }
    if (!save_transaction(selected,transaction,fresh,old_inf,property(set.value,info,SPDRP_SERVICE))) {
        return with_recovery(failure(L"Cannot anchor recovery hashes in the protected machine journal. No binding was changed."),transaction);
    }
    const auto firmware_dir = join(fresh.windows,L"system32\\drivers\\rtl_nic");
    if (!CreateDirectoryW(firmware_dir.c_str(),nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return with_recovery(failure(L"Cannot create the firmware folder."),transaction);
    }
    const auto flags = GetFileAttributesW(firmware_dir.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return with_recovery(failure(L"The firmware destination is not a regular directory.",ERROR_ACCESS_DENIED),transaction);
    }
    if ((!equal(source_b,installed_firmware(fresh,true)) && !CopyFileW(source_b.c_str(),installed_firmware(fresh,true).c_str(),FALSE)) ||
        (need_a && !equal(source_a,installed_firmware(fresh,false)) && !CopyFileW(source_a.c_str(),installed_firmware(fresh,false).c_str(),FALSE))) {
        return with_recovery(failure(L"Firmware could not be copied. Backups are retained."),transaction);
    }
    if (progress != nullptr) { progress(context,3); }
    if (!select_inf(set.value,info,join(fresh.package,L"rtl8125.inf"),card_name(selected.card)) || !install_selected(set.value,info)) {
        return with_recovery(failure(L"ReactOS could not bind the selected driver. Use Restore or the saved recovery files."),transaction);
    }
    if (progress != nullptr) { progress(context,4); }
    const auto* label = card_name(selected.card);
    if (!SetupDiSetDeviceRegistryPropertyW(set.value,&info,SPDRP_FRIENDLYNAME,
            reinterpret_cast<const BYTE*>(label),static_cast<DWORD>((std::wcslen(label)+1)*2)) ||
        property(set.value,info,SPDRP_FRIENDLYNAME) != label || !equal(property(set.value,info,SPDRP_SERVICE),L"Rtl8125")) {
        return with_recovery(failure(L"The device binding or displayed name could not be verified.",ERROR_INVALID_DATA),transaction);
    }
    SP_DEVINSTALL_PARAMS_W parameters{};
    parameters.cbSize = sizeof(parameters);
    if (!SetupDiGetDeviceInstallParamsW(set.value,&info,&parameters)) { return with_recovery(failure(L"Cannot read the installation result."),transaction); }
    const bool reboot = (parameters.Flags & (DI_NEEDREBOOT|DI_NEEDRESTART)) != 0;
    ULONG status{}, problem{};
    if (!reboot && (CM_Get_DevNode_Status(&status,&problem,info.DevInst,0) != CR_SUCCESS || problem != 0 || (status & DN_STARTED) == 0)) {
        return with_recovery(failure(L"The driver is configured, but Device Manager reports a start problem. Restart or use Restore.",ERROR_DEVICE_NOT_AVAILABLE),transaction);
    }
    return with_recovery({true,reboot,false,0,reboot ? L"Driver configured. Restart ReactOS to finish installation." :
        L"Driver installed. Device Manager now identifies your card correctly. Connect an Ethernet cable and check network settings.",{}},transaction);
}

Result remove(const Snapshot& snapshot, const Device& selected)
{
    if (snapshot.preview) { return {true,false,true,0,L"Preview only. No device was removed.",{}}; }
    if (is_wine()) { return failure(L"Driver removal is unavailable in Wine.",ERROR_ACCESS_DENIED); }
    const auto fresh = inspect(false);
    if (fresh.state != Compatibility::ready) { return failure(L"Use the compatible ReactOS image or the offline recovery instructions.",ERROR_REVISION_MISMATCH); }
    DeviceSet set;
    SP_DEVINFO_DATA info{};
    if (!open_selected(set,info,selected) || !equal(property(set.value,info,SPDRP_SERVICE),L"Rtl8125")) { return failure(L"This adapter is not using this driver.",ERROR_INVALID_DATA); }
    if (!disable(set.value,info)) { return failure(L"ReactOS could not stop the selected adapter. No files were removed."); }
    if (!SetupDiCallClassInstaller(DIF_REMOVE,set.value,&info)) {
        return failure(L"The adapter was disabled. This ReactOS build could not uninstall the device; its removal API may be unavailable. Use Restore or Device Manager. Driver and recovery files were preserved.");
    }
    DeviceSet verify;
    SP_DEVINFO_DATA verify_info{};
    if (open_selected(verify,verify_info,selected) && equal(property(verify.value,verify_info,SPDRP_SERVICE),L"Rtl8125")) {
        return failure(L"ReactOS returned without removing the driver binding. The adapter remains disabled.",ERROR_CALL_NOT_IMPLEMENTED);
    }
    return {true,true,false,0,L"The selected device binding was removed. Restart ReactOS. Shared driver files and recovery evidence were preserved.",transaction_for(fresh,selected)};
}

Result restore(const Snapshot& snapshot, const Device& selected)
{
    if (snapshot.preview) { return {true,false,true,0,L"Preview only. No recovery files were restored.",{}}; }
    if (is_wine()) { return failure(L"Recovery is unavailable in Wine.",ERROR_ACCESS_DENIED); }
    const auto fresh = inspect(false);
    if (fresh.state != Compatibility::ready) { return failure(L"Use the compatible ReactOS image or recover the stopped guest offline.",ERROR_REVISION_MISMATCH); }
    const auto transaction = transaction_for(fresh,selected);
    if (transaction.empty()) { return failure(L"No protected recovery journal exists for this card.",ERROR_FILE_NOT_FOUND); }
    for (const auto& device : fresh.devices) {
        if (!equal(device.instance,selected.instance) && equal(device.service,L"Rtl8125")) {
            return with_recovery(failure(L"Another adapter uses these shared driver files. Restore the guest snapshot to recover both safely.",ERROR_BUSY),transaction);
        }
    }
    DeviceSet set;
    SP_DEVINFO_DATA info{};
    if (!open_selected(set,info,selected) || !equal(property(set.value,info,SPDRP_SERVICE),L"Rtl8125") || !disable(set.value,info)) {
        return with_recovery(failure(L"The selected adapter could not be stopped. Use the offline recovery instructions."),transaction);
    }
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,device_key(selected).c_str(),0,KEY_QUERY_VALUE|READ_CONTROL,&key.value) != ERROR_SUCCESS ||
        !journal_usable(key.value)) { return with_recovery(failure(L"The recovery journal is not protected.",ERROR_ACCESS_DENIED),transaction); }
    const auto previous_service=registry_string(key.value,L"PreviousService");
    const auto previous_binary_name=registry_string(key.value,L"PreviousBinary");
    const auto expected_binary_hash=registry_string(key.value,L"PreviousBinaryHash");
    if (previous_service.empty() || equal(previous_service,L"Rtl8125") || !safe_component(previous_binary_name) || expected_binary_hash.size()!=64) {
        return with_recovery(failure(L"The adapter is disabled. Restoring this earlier driver requires the stopped guest and saved backup files; setup will not overwrite a loaded driver.",ERROR_NOT_SUPPORTED),transaction);
    }
    if (registry_string(key.value,L"previous.inf").size()!=64) {
        return with_recovery(failure(L"No verified previous INF was recorded. Keep the adapter disabled and use offline recovery.",ERROR_INVALID_DATA),transaction);
    }
    std::array<Handle,4> locks;
    unsigned index{};
    for (const auto* file : {L"previous.inf",L"rtl8125.sys.before",L"rtl8125a-3.fw.before",L"rtl8125b-2.fw.before"}) {
        const auto expected=registry_string(key.value,file);
        if (!expected.empty()) {
            const auto path=join(transaction,file);
            locks[index].value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            std::string actual;
            if (locks[index].value==INVALID_HANDLE_VALUE || !file_hash(path,actual) || wide(actual)!=expected) {
                return with_recovery(failure(L"Recovery file integrity failed. Keep the adapter disabled and use a trusted guest snapshot.",ERROR_INVALID_DATA),transaction);
            }
        }
        ++index;
    }
    const auto binary=join(join(fresh.windows,L"system32\\drivers"),previous_binary_name.c_str());
    Handle old_binary(CreateFileW(binary.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    std::string actual_binary_hash;
    if (old_binary.value==INVALID_HANDLE_VALUE || !file_hash(binary,actual_binary_hash) || wide(actual_binary_hash)!=expected_binary_hash) {
        return with_recovery(failure(L"The previous driver binary changed. Restore the stopped guest using the saved files.",ERROR_REVISION_MISMATCH),transaction);
    }
    const auto previous = join(transaction,L"previous.inf");
    if (GetFileAttributesW(previous.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return with_recovery(failure(L"There was no previous driver to restore. The adapter is disabled; use Remove or the offline recovery instructions.",ERROR_NOT_FOUND),transaction);
    }
    if (!select_inf(set.value,info,previous,nullptr,true) || !SetupDiCallClassInstaller(DIF_INSTALLDEVICE,set.value,&info) ||
        !equal(property(set.value,info,SPDRP_SERVICE),previous_service)) {
        return with_recovery(failure(L"ReactOS could not restore the previous binding. The protected backups remain available for offline recovery."),transaction);
    }
    const auto name=registry_string(key.value,L"PreviousName");
    if (!SetupDiSetDeviceRegistryPropertyW(set.value,&info,SPDRP_FRIENDLYNAME,
            name.empty()?nullptr:reinterpret_cast<const BYTE*>(name.c_str()),static_cast<DWORD>(name.empty()?0:(name.size()+1)*2)) ||
        !enable(set.value,info)) { return with_recovery(failure(L"The previous binding was restored, but ReactOS could not enable it. Restart and check Device Manager."),transaction); }
    return with_recovery({true,true,false,0,L"Previous driver binding restored. Restart ReactOS. Firmware backups remain in the recovery folder.",{}},transaction);
}
}
