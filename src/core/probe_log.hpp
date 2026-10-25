// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.probe_log
Purpose: Validate complete bounded native diagnostic attempts from serial text.
Created: 2026-10-24
Architecture: Offline parser using chip/resource rules; never certifies log origin.
*/
#pragma once

#include "chip.hpp"
#include <cstddef>
#include <cstdint>

namespace rtl8125 {

inline constexpr std::size_t maximum_probe_line_size = 512;
inline constexpr std::size_t maximum_probe_capture_size = 64 * 1024 * 1024;

struct ProbeLogRecord {
    std::uint8_t revision{};
    std::uint8_t bar_index{};
    std::uint64_t address{};
    std::uint32_t length{};
    std::uint32_t interrupt_vector{};
    std::uint32_t interrupt_level{};
    std::uint16_t interrupt_flags{};
    std::uint32_t tx_config{};
    ChipIdentity chip{ChipStatus::unknown_mac, MacVersion::unsupported, 0};
    std::uint32_t final_status{};
};

// No allocation or device access. Feed one complete line without LF; a final CR
// is accepted. Each record requires v1 begin, consecutive resource/IRQ/XID lines,
// then the matching fail-closed end status. Any other intervening line discards
// the partial attempt. A new begin always starts from empty state. This prevents
// joining fields across logged attempt boundaries; lost/forged log contents and
// the actual origin of a serial capture cannot be established by a text parser.
class ProbeLogParser {
public:
    void consume_line(const char* line, std::size_t length) noexcept;
    void finish() noexcept;
    std::size_t completed_count() const noexcept { return completed_count_; }
    std::size_t rejected_count() const noexcept { return rejected_count_; }
    const ProbeLogRecord& last_record() const noexcept { return last_record_; }

private:
    enum class Stage { idle, resources, interrupt, identity, end };
    Stage stage_{Stage::idle};
    ProbeLogRecord candidate_{};
    ProbeLogRecord last_record_{};
    std::size_t completed_count_{};
    std::size_t rejected_count_{};
    void discard() noexcept;
};

// Matches only observed stable identity fields from the actual Linux snapshot:
// PCI revision 05, BAR2, RTL8125B XID641. Addresses may be reassigned on reboot.
// This is a consistency check, not proof of ReactOS or physical execution.
bool matches_linux_card(const ProbeLogRecord& record) noexcept;

} // namespace rtl8125
