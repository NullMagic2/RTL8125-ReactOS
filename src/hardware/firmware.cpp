// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.hardware.firmware
Purpose: Checked Linux firmware format, control flow and bounded interpreter.
Created: 2026-10-24
Architecture: No allocations, file I/O or raw structure casts.
Derived from Linux v6.12 r8169_firmware.c, ShuChen and Francois Romieu et al.
*/
#include "firmware.hpp"

namespace rtl8125::hardware {
namespace {
std::uint32_t little32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint32_t instruction(const FirmwareView& view, std::size_t index) noexcept
{
    return little32(view.bytes + view.code_offset + index * 4);
}
}

FirmwareStatus validate_firmware(const std::uint8_t* bytes, std::size_t size,
                                  FirmwareView& output) noexcept
{
    output = {};
    if (bytes == nullptr || size < 4 || size > maximum_firmware_size) {
        return FirmwareStatus::invalid_format;
    }
    FirmwareView view{};
    view.bytes = bytes;
    view.byte_count = size;
    if (little32(bytes) == 0) {
        constexpr std::size_t header_size = 45;
        if (size < header_size) {
            return FirmwareStatus::invalid_format;
        }
        std::uint8_t checksum{};
        for (std::size_t index = 0; index < size; ++index) {
            checksum = static_cast<std::uint8_t>(checksum + bytes[index]);
        }
        if (checksum != 0) {
            return FirmwareStatus::checksum;
        }
        view.code_offset = little32(bytes + 36);
        view.instruction_count = little32(bytes + 40);
        if (view.code_offset < header_size || view.code_offset > size ||
            view.instruction_count > (size - view.code_offset) / 4) {
            return FirmwareStatus::invalid_format;
        }
        for (std::size_t index = 0; index < sizeof(view.version) - 1; ++index) {
            view.version[index] = static_cast<char>(bytes[4 + index]);
            if (view.version[index] == 0) {
                break;
            }
        }
    } else {
        if (size % 4 != 0) {
            return FirmwareStatus::invalid_format;
        }
        view.instruction_count = size / 4;
    }
    if (view.instruction_count == 0) {
        return FirmwareStatus::invalid_format;
    }
    for (std::size_t index = 0; index < view.instruction_count; ++index) {
        const auto action = instruction(view, index);
        const auto value = action & 0xffff;
        const auto reg = (action >> 16) & 0xfff;
        switch (action >> 28) {
        case 0: case 1: case 2: case 7: case 8: case 12: case 14:
            break;
        case 4:
            if (value > 1) {
                return FirmwareStatus::opcode;
            }
            break;
        case 3:
            if (reg > index) {
                return FirmwareStatus::branch;
            }
            break;
        case 9:
            if (view.instruction_count - index <= 2) {
                return FirmwareStatus::branch;
            }
            break;
        case 10: case 11: case 13:
            if (reg >= view.instruction_count - index - 1) {
                return FirmwareStatus::branch;
            }
            break;
        default:
            return FirmwareStatus::opcode;
        }
    }
    output = view;
    return FirmwareStatus::ok;
}

FirmwareResult execute_firmware(const FirmwareView& input, RegisterSession& session,
                                 std::uint32_t maximum_steps) noexcept
{
    FirmwareView view{};
    auto status = validate_firmware(input.bytes, input.byte_count, view);
    if (status != FirmwareStatus::ok) {
        return {status, 0, 0};
    }
    if (maximum_steps == 0 || maximum_steps > 1000000) {
        return {FirmwareStatus::budget_exhausted, 0, 0};
    }
    std::uint16_t previous{};
    std::uint32_t read_count{};
    std::uint32_t steps{};
    std::size_t index{};
    bool mcu{};
    while (index < view.instruction_count) {
        if (steps == maximum_steps) {
            return {FirmwareStatus::budget_exhausted, steps, index};
        }
        if (!session.checkpoint()) {
            return {session.status() == Status::budget_exhausted ? FirmwareStatus::budget_exhausted :
                    FirmwareStatus::io_error, steps, index};
        }
        ++steps;
        const auto action = instruction(view, index);
        const auto data = static_cast<std::uint16_t>(action);
        const auto reg = static_cast<std::uint16_t>((action >> 16) & 0xfff);
        bool success = true;
        auto next = index + 1;
        switch (action >> 28) {
        case 0:
            success = mcu ? session.mcu_read(reg, previous) : session.mdio_read(reg, previous);
            ++read_count;
            break;
        case 1: previous |= data; break;
        case 2: previous &= data; break;
        case 3: next = index - reg; break;
        case 4: mcu = data != 0; break;
        case 7: read_count = 0; break;
        case 8:
            success = mcu ? session.mcu_write(reg, data) : session.mdio_write(reg, data);
            break;
        case 9: if (read_count == data) { ++next; } break;
        case 10: if (previous == data) { next += reg; } break;
        case 11: if (previous != data) { next += reg; } break;
        case 12:
            success = mcu ? session.mcu_write(reg, previous) : session.mdio_write(reg, previous);
            break;
        case 13: next += reg; break;
        case 14: success = session.delay(static_cast<std::uint32_t>(data) * 1000); break;
        default: return {FirmwareStatus::opcode, steps, index};
        }
        if (!success) {
            return {session.status() == Status::budget_exhausted ? FirmwareStatus::budget_exhausted :
                    FirmwareStatus::io_error, steps, index};
        }
        index = next;
    }
    return {FirmwareStatus::ok, steps, index};
}

} // namespace rtl8125::hardware
