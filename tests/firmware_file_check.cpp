// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.firmware_file_check
Purpose: Validate a separately acquired firmware file without executing its code.
Created: 2026-10-24
Architecture: Host-only bounded file input; no register callbacks or device I/O.
*/
#include "../src/hardware/firmware.hpp"
#include <array>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    auto* file = std::fopen(argv[1], "rb");
    if (file == nullptr) { return 2; }
    static std::array<std::uint8_t, rtl8125::hardware::maximum_firmware_size + 1> bytes{};
    const auto count = std::fread(bytes.data(), 1, bytes.size(), file);
    const auto error = std::ferror(file);
    std::fclose(file);
    if (error != 0) { return 2; }
    rtl8125::hardware::FirmwareView view{};
    const auto status = rtl8125::hardware::validate_firmware(bytes.data(), count, view);
    std::printf("Firmware format only: status=%u bytes=%zu instructions=%zu offset=%zu version=%s\n",
        static_cast<unsigned>(status), count, view.instruction_count, view.code_offset, view.version);
    return status == rtl8125::hardware::FirmwareStatus::ok ? 0 : 1;
}
