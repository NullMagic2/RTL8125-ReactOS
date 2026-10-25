// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.miniport_guest_runner
Purpose: Load/unload the isolated kernel fixture and report service results.
Created: 2026-10-24
Architecture: Guest-only C++17 Win32 service controller; no device installation.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace {

struct Log {
    HANDLE serial{INVALID_HANDLE_VALUE};
    FILE* file{};

    ~Log() noexcept
    {
        if (serial != INVALID_HANDLE_VALUE) {
            CloseHandle(serial);
        }

        if (file != nullptr) {
            std::fclose(file);
        }
    }

    void line(const char* text) const noexcept
    {
        std::printf("%s\r\n", text);
        OutputDebugStringA(text);
        OutputDebugStringA("\n");

        if (file != nullptr) {
            std::fprintf(file, "%s\r\n", text);
            std::fflush(file);
        }

        if (serial != INVALID_HANDLE_VALUE) {
            DWORD written{};
            WriteFile(serial, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
            WriteFile(serial, "\r\n", 2, &written, nullptr);
        }
    }

    void status(const char* action, DWORD result) const noexcept
    {
        char text[160]{};
        std::snprintf(text, sizeof(text), "RTL8125_M01_RUNNER: %s result %lu", action, result);
        line(text);
    }
};

} // namespace

int main()
{
    Log log;
    log.file = std::fopen("C:\\RTL8125-M01-GUEST.txt", "wb");
    log.serial = CreateFileA("\\\\.\\COM2", GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);

    if (log.serial != INVALID_HANDLE_VALUE) {
        DCB configuration{};
        configuration.DCBlength = sizeof(configuration);
        configuration.BaudRate = CBR_115200;
        configuration.fBinary = TRUE;
        configuration.ByteSize = 8;
        configuration.StopBits = ONESTOPBIT;
        SetCommState(log.serial, &configuration);
        COMMTIMEOUTS timeouts{};
        timeouts.WriteTotalTimeoutConstant = 2000;
        SetCommTimeouts(log.serial, &timeouts);
    }
    log.line("RTL8125_M01_RUNNER: begin v1 isolated guest fixture, no hardware binding");
    OSVERSIONINFOA version{};
    version.dwOSVersionInfoSize = sizeof(version);
    GetVersionExA(&version);
    char version_line[192]{};
    std::snprintf(version_line, sizeof(version_line),
                  "RTL8125_M01_RUNNER: OS %lu.%lu build %lu %s", version.dwMajorVersion,
                  version.dwMinorVersion, version.dwBuildNumber, version.szCSDVersion);
    log.line(version_line);

    char source[MAX_PATH]{};
    char target[MAX_PATH]{};
    const auto source_length = GetModuleFileNameA(nullptr, source, sizeof(source));

    if (source_length == 0 || source_length >= sizeof(source)) {
        log.status("module-path-failed", GetLastError());
        return 1;
    }

    auto* separator = std::strrchr(source, '\\');

    if (separator == nullptr || static_cast<std::size_t>(separator - source) +
        sizeof("\\rtl8125_m01_guest.sys") > sizeof(source)) {
        return 1;
    }

    std::snprintf(separator, sizeof(source) - static_cast<std::size_t>(separator - source),
                  "%s", "\\rtl8125_m01_guest.sys");
    const auto system_length = GetSystemDirectoryA(target, sizeof(target));

    if (system_length == 0 || system_length + sizeof("\\drivers\\rtl8125_m01_guest.sys") >
        sizeof(target)) {
        return 1;
    }

    std::snprintf(target + system_length, sizeof(target) - system_length,
                  "%s", "\\drivers\\rtl8125_m01_guest.sys");

    if (!CopyFileA(source, target, FALSE)) {
        log.status("copy-failed", GetLastError());
        return 1;
    }

    const auto manager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);

    if (manager == nullptr) {
        log.status("manager-open-failed", GetLastError());
        return 1;
    }

    auto service = CreateServiceA(manager, "RTL8125M01Guest", "RTL8125 M01 isolated kernel fixture",
                                  SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE,
                                  SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                                  SERVICE_ERROR_NORMAL, target, nullptr, nullptr, nullptr,
                                  nullptr, nullptr);

    if (service == nullptr && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceA(manager, "RTL8125M01Guest",
                               SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    }

    if (service == nullptr) {
        log.status("create-service-failed", GetLastError());
        CloseServiceHandle(manager);
        return 1;
    }

    unsigned failures{};

    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        const auto started = StartServiceA(service, 0, nullptr);
        const auto start_error = started ? ERROR_SUCCESS : GetLastError();
        log.status("start-service", start_error);

        if (!started) {
            ++failures;
            break;
        }

        SERVICE_STATUS state{};
        const auto queried = QueryServiceStatus(service, &state);
        log.status("query-service-state", queried ? state.dwCurrentState : GetLastError());

        if (!queried || state.dwCurrentState != SERVICE_RUNNING) {
            ++failures;
        }

        const auto stopped = ControlService(service, SERVICE_CONTROL_STOP, &state);
        log.status("stop-service", stopped ? ERROR_SUCCESS : GetLastError());

        if (!stopped) {
            ++failures;
            break;
        }
    }

    const auto deleted = DeleteService(service);
    log.status("delete-service", deleted ? ERROR_SUCCESS : GetLastError());
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    log.status("end-failures", failures);

    return failures == 0 && deleted ? 0 : 1;
}
