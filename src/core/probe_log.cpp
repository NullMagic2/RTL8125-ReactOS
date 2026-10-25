// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.probe_log
Purpose: Parse exact versioned debug lines without overreads or mixed attempts.
Created: 2026-10-24
Architecture: Implements probe_log.hpp using immutable text and production checks.
*/
#include "probe_log.hpp"
#include "resources.hpp"

namespace rtl8125 {
namespace {

class Cursor {
public:
    Cursor(const char* text, std::size_t length) noexcept : text_(text), length_(length) {}

    bool token(const char* token_text) noexcept
    {
        for (std::size_t index = 0; token_text[index] != '\0'; ++index) {
            if (position_ == length_ || text_[position_] != token_text[index]) {
                return false;
            }

            ++position_;
        }

        return true;
    }

    bool number(unsigned base, std::uint32_t& value) noexcept
    {
        value = 0;
        const auto begin = position_;

        while (position_ < length_) {
            const auto character = text_[position_];
            unsigned digit = 16;

            if (character >= '0' && character <= '9') {
                digit = static_cast<unsigned>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                digit = static_cast<unsigned>(character - 'a') + 10;
            } else if (character >= 'A' && character <= 'F') {
                digit = static_cast<unsigned>(character - 'A') + 10;
            }

            if (digit >= base) {
                break;
            }

            if (value > (UINT32_MAX - digit) / base) {
                return false;
            }

            value = value * base + digit;
            ++position_;
        }

        return position_ != begin;
    }

    bool end() const noexcept { return position_ == length_; }

private:
    const char* text_;
    std::size_t length_;
    std::size_t position_{};
};

bool parse_resources(Cursor& cursor, ProbeLogRecord& record) noexcept
{
    std::uint32_t revision{};
    std::uint32_t bar{};
    std::uint32_t high{};
    std::uint32_t low{};

    if (!cursor.token("RTL8125: rev ") || !cursor.number(16, revision) ||
        !cursor.token(" BAR") || !cursor.number(10, bar) ||
        !cursor.token(" address ") || !cursor.number(16, high) ||
        !cursor.token(":") || !cursor.number(16, low) ||
        !cursor.token(" length ") || !cursor.number(16, record.length) || !cursor.end() ||
        revision > 255 || bar > 5 || record.length < minimum_register_aperture) {
        return false;
    }

    record.revision = static_cast<std::uint8_t>(revision);
    record.bar_index = static_cast<std::uint8_t>(bar);
    record.address = (static_cast<std::uint64_t>(high) << 32) | low;
    return record.address != 0 && (record.address & 15U) == 0 &&
           record.address <= UINT64_MAX - (record.length - 1U) &&
           valid_mmio_access(record.length, tx_config_offset, 4);
}

bool parse_interrupt(Cursor& cursor, ProbeLogRecord& record) noexcept
{
    std::uint32_t flags{};

    if (!cursor.token("RTL8125: IRQ vector ") || !cursor.number(10, record.interrupt_vector) ||
        !cursor.token(" level ") || !cursor.number(10, record.interrupt_level) ||
        !cursor.token(" flags ") || !cursor.number(16, flags) ||
        !cursor.token(" TxConfig ") || !cursor.number(16, record.tx_config) || !cursor.end() ||
        record.interrupt_vector == 0 || record.interrupt_level == 0 || flags > 1) {
        return false;
    }

    record.interrupt_flags = static_cast<std::uint16_t>(flags);
    record.chip = identify_chip(realtek_vendor_id, rtl8125_device_id, record.tx_config);
    return record.chip.status == ChipStatus::supported;
}

bool parse_identity(Cursor& cursor, const ProbeLogRecord& record) noexcept
{
    std::uint32_t xid{};
    std::uint32_t family{};
    return cursor.token("RTL8125: probe XID ") && cursor.number(16, xid) &&
           cursor.token(", MAC family ") && cursor.number(10, family) &&
           cursor.token("; datapath incomplete") && cursor.end() &&
           xid == record.chip.xid && family == static_cast<std::uint32_t>(record.chip.version);
}

} // namespace

void ProbeLogParser::discard() noexcept
{
    if (stage_ != Stage::idle) {
        ++rejected_count_;
    }

    candidate_ = {};
    stage_ = Stage::idle;
}

void ProbeLogParser::consume_line(const char* line, std::size_t length) noexcept
{
    if (line == nullptr || length > maximum_probe_line_size) {
        discard();
        return;
    }

    if (length != 0 && line[length - 1] == '\r') {
        --length;
    }

    for (std::size_t index = 0; index < length; ++index) {
        if (line[index] == '\0' || line[index] == '\n' || line[index] == '\r') {
            discard();
            return;
        }
    }

    Cursor begin(line, length);

    if (begin.token("RTL8125: probe begin v1") && begin.end()) {
        discard();
        stage_ = Stage::resources;
        return;
    }

    Cursor cursor(line, length);
    bool valid = false;

    switch (stage_) {
    case Stage::idle: return;
    case Stage::resources:
        valid = parse_resources(cursor, candidate_);
        stage_ = valid ? Stage::interrupt : stage_;
        break;
    case Stage::interrupt:
        valid = parse_interrupt(cursor, candidate_);
        stage_ = valid ? Stage::identity : stage_;
        break;
    case Stage::identity:
        valid = parse_identity(cursor, candidate_);
        stage_ = valid ? Stage::end : stage_;
        break;
    case Stage::end:
        valid = cursor.token("RTL8125: probe end status ") &&
                cursor.number(16, candidate_.final_status) && cursor.end() &&
                candidate_.final_status == 0xc00000bbU; // NDIS_STATUS_NOT_SUPPORTED.

        if (valid) {
            last_record_ = candidate_;
            ++completed_count_;
            candidate_ = {};
            stage_ = Stage::idle;
        }

        break;
    }

    if (!valid) {
        discard();
    }
}

void ProbeLogParser::finish() noexcept
{
    discard();
}

bool matches_linux_card(const ProbeLogRecord& record) noexcept
{
    return record.revision == 5 && record.bar_index == 2 &&
           record.chip.status == ChipStatus::supported &&
           record.chip.version == MacVersion::rtl8125b && record.chip.xid == 0x641;
}

} // namespace rtl8125
