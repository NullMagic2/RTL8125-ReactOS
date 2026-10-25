/*
Module: rtl8125.validation.shared_network
Purpose: Inspect NetKVM and verify a transfer bound to the VM's VirtIO adapter.
Created: 2026-10-24
Architecture: User-mode diagnostic only; never changes host bindings or guest drivers.
*/
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

void report(const std::string& value)
{
    const auto line = "SHARED_NET " + value + "\n";
    std::fputs(line.c_str(), stdout);
    OutputDebugStringA(line.c_str());
}

int main(int argc, char** argv)
{
    report("BEGIN actual guest inspection");
    wchar_t system[MAX_PATH]{};
    if (GetSystemDirectoryW(system, MAX_PATH) == 0) { return 1; }
    const auto path = std::wstring(system) + L"\\drivers\\netkvm.sys";
    report("BundledNetKvmPresent=" + std::to_string(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES));
    const GUID network_class{0x4d36e972,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
    const auto devices = SetupDiGetClassDevsW(&network_class, nullptr, nullptr, DIGCF_PRESENT);
    bool started = false;
    if (devices != INVALID_HANDLE_VALUE) {
        for (DWORD i = 0;; ++i) {
            SP_DEVINFO_DATA device{};
            device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(devices, i, &device)) { break; }
            wchar_t instance[1024]{};
            if (!SetupDiGetDeviceInstanceIdW(devices, &device, instance, 1024, nullptr)) { continue; }
            std::wstring id(instance);
            std::transform(id.begin(), id.end(), id.begin(), towupper);
            if (id.find(L"PCI\\VEN_1AF4&DEV_1000") != 0) { continue; }
            wchar_t service[128]{};
            DWORD type{};
            SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_SERVICE, &type,
                reinterpret_cast<BYTE*>(service), sizeof(service), nullptr);
            ULONG status{}, problem{};
            const auto result = CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0);
            const bool matched = _wcsicmp(service, L"netkvm") == 0;
            started = matched && result == CR_SUCCESS && (status & DN_STARTED) != 0 && problem == 0;
            report("VirtioPnp NetKvm=" + std::to_string(matched) + " Started=" + std::to_string(started) + " Problem=" + std::to_string(problem));
        }
        SetupDiDestroyDeviceInfoList(devices);
    }
    ULONG size{};
    const auto initial = GetAdaptersInfo(nullptr, &size);
    if (initial != ERROR_BUFFER_OVERFLOW || size == 0 || size > 1024 * 1024) {
        report("AdapterQueryError=" + std::to_string(initial));
        return 2;
    }
    std::vector<BYTE> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(storage.data());
    if (GetAdaptersInfo(adapters, &size) != ERROR_SUCCESS) { return 2; }
    std::string source;
    DWORD virtio_index{};
    const BYTE expected_mac[]{0x52,0x54,0x00,0x81,0x25,0x01};
    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        report("Adapter=" + std::string(adapter->Description) + " IPv4=" + adapter->IpAddressList.IpAddress.String);
        if (adapter->AddressLength == sizeof(expected_mac) && std::memcmp(adapter->Address, expected_mac, sizeof(expected_mac)) == 0) {
            virtio_index = adapter->Index;
            for (auto* ip = &adapter->IpAddressList; ip != nullptr; ip = ip->Next) {
                if (std::strncmp(ip->IpAddress.String, "10.0.3.", 7) == 0) { source = ip->IpAddress.String; }
            }
        }
    }
    report("VirtioSourceIPv4=" + source);
    DWORD best{};
    const auto route_status = GetBestInterface(inet_addr("1.1.1.1"), &best);
    report("VirtioDefaultRoute=" + std::to_string(route_status == NO_ERROR && virtio_index != 0 && best == virtio_index));
    if (argc != 2) { return 0; }
    if (!started || source.empty() || std::strlen(argv[1]) != 64) { return 3; }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data) != 0) { return 4; }
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) { WSACleanup(); return 4; }
    sockaddr_in local{}, remote{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = inet_addr(source.c_str());
    remote.sin_family = AF_INET;
    remote.sin_port = htons(18425);
    remote.sin_addr.s_addr = inet_addr("10.0.3.2");
    bool ok = bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
    u_long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);
    if (ok && connect(socket, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { ok = false; }
        else {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(socket, &writable);
            timeval timeout{5,0};
            int error{}, bytes = sizeof(error);
            ok = select(0, nullptr, &writable, nullptr, &timeout) == 1 &&
                getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &bytes) == 0 && error == 0;
        }
    }
    nonblocking = 0;
    ioctlsocket(socket, FIONBIO, &nonblocking);
    DWORD timeout = 5000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    const char request[] = "GET /token.txt HTTP/1.0\r\nHost: 10.0.3.2\r\n\r\n";
    std::size_t sent{};
    while (ok && sent < sizeof(request)-1) {
        const auto count = send(socket, request+sent, static_cast<int>(sizeof(request)-1-sent), 0);
        if (count <= 0) { ok = false; } else { sent += count; }
    }
    std::string response;
    char buffer[2048];
    while (ok && response.size() < 32768) {
        const auto count = recv(socket, buffer, sizeof(buffer), 0);
        if (count == 0) { break; }
        if (count < 0) { ok = false; break; }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    closesocket(socket);
    WSACleanup();
    const auto boundary = response.find("\r\n\r\n");
    const bool passed = ok && response.find("HTTP/1.0 200 ") == 0 && boundary != std::string::npos &&
        response.substr(boundary+4) == std::string(argv[1])+"\n";
    report("VirtioBoundHttpTransfer=" + std::to_string(passed) + " Bytes=" + std::to_string(response.size()));
    report("END shared networking only; RTL8125 hardware not exercised");
    return passed ? 0 : 5;
}
