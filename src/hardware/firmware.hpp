// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.hardware.firmware
Purpose: Validate and execute Linux Realtek firmware without trusting file bytes.
Created: 2026-10-24
Architecture: Borrowed immutable input and an exclusive bounded RegisterSession.
Derived from Linux v6.12 r8169_firmware.c, ShuChen and Francois Romieu et al.
*/
#pragma once
#include "registers.hpp"

namespace rtl8125::hardware {

inline constexpr std::size_t maximum_firmware_size = 1024 * 1024;
enum class FirmwareStatus { ok, invalid_format, checksum, opcode, branch, io_error, budget_exhausted };

struct FirmwareView {
    const std::uint8_t* bytes{};
    std::size_t byte_count{};
    std::size_t code_offset{};
    std::size_t instruction_count{};
    char version[32]{};
};

struct FirmwareResult {
    FirmwareStatus status{FirmwareStatus::invalid_format};
    std::uint32_t steps{};
    std::size_t instruction{};
};

// Any IRQL, bounded by the 1-MiB file cap. Clears output on every failure.
// The input must remain immutable and alive through execution. Headered and raw
// upstream formats are decoded bytewise, so file alignment is never assumed.
FirmwareStatus validate_firmware(const std::uint8_t*, std::size_t, FirmwareView&) noexcept;

// PASSIVE_LEVEL natively, caller holds an exclusive session. Revalidates the
// complete file before the first device write. All Linux opcodes are supported;
// backward loops consume a separate step cap, and delays consume session time.
// Failure leaves hardware possibly modified: caller must quiesce/reset; it must
// never assume firmware rollback or advertise a usable PHY after failure.
FirmwareResult execute_firmware(const FirmwareView&, RegisterSession&,
                                std::uint32_t maximum_steps = 262144) noexcept;

} // namespace rtl8125::hardware
