// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tools.probe_collect
Purpose: Collect bounded native probe serial text into an honest evidence record.
Created: 2026-10-24
Architecture: Host command-line utility over core probe_log; never touches devices.
*/
#include "../core/probe_log.hpp"

#include <cstdio>
#include <cstring>

namespace {

bool read_capture(std::FILE* input, rtl8125::ProbeLogParser& parser) noexcept
{
    char line[rtl8125::maximum_probe_line_size]{};
    std::size_t used{};
    std::size_t total{};
    bool overflow{};
    int character{};

    while ((character = std::fgetc(input)) != EOF) {
        if (++total > rtl8125::maximum_probe_capture_size) {
            return false;
        }

        if (character == '\n') {
            parser.consume_line(overflow ? nullptr : line, used);
            used = 0;
            overflow = false;
        } else if (used < sizeof(line)) {
            line[used++] = static_cast<char>(character);
        } else {
            overflow = true;
        }
    }

    if (used != 0 || overflow) {
        parser.consume_line(overflow ? nullptr : line, used);
    }

    parser.finish();
    return std::ferror(input) == 0;
}

bool write_record(std::FILE* output, const rtl8125::ProbeLogParser& parser) noexcept
{
    const auto& record = parser.last_record();
    const auto written = std::fprintf(output,
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"capture_origin\": \"external_serial_log_not_independently_verified\",\n"
        "  \"reactos_physical_gate_passed\": false,\n"
        "  \"selection\": \"last_complete_probe_attempt\",\n"
        "  \"complete_attempts\": %zu,\n"
        "  \"discarded_attempts\": %zu,\n"
        "  \"matches_known_linux_card\": %s,\n"
        "  \"linux_identity_evidence\": \"evidence/m02-linux-device-probe.txt\",\n"
        "  \"pci_revision\": \"%02x\",\n"
        "  \"bar_index\": %u,\n"
        "  \"physical_address\": \"%08x:%08x\",\n"
        "  \"aperture_bytes\": %u,\n"
        "  \"assigned_interrupt_vector\": %u,\n"
        "  \"assigned_interrupt_level\": %u,\n"
        "  \"assigned_interrupt_flags\": %u,\n"
        "  \"tx_config\": \"%08x\",\n"
        "  \"xid\": \"%03x\",\n"
        "  \"mac_family\": \"%s\",\n"
        "  \"initialize_status\": \"%08x\",\n"
        "  \"datapath_ready\": false\n"
        "}\n",
        parser.completed_count(), parser.rejected_count(),
        rtl8125::matches_linux_card(record) ? "true" : "false",
        static_cast<unsigned>(record.revision), static_cast<unsigned>(record.bar_index),
        static_cast<unsigned>(record.address >> 32), static_cast<unsigned>(record.address),
        static_cast<unsigned>(record.length), static_cast<unsigned>(record.interrupt_vector),
        static_cast<unsigned>(record.interrupt_level), static_cast<unsigned>(record.interrupt_flags),
        static_cast<unsigned>(record.tx_config), static_cast<unsigned>(record.chip.xid),
        record.chip.version == rtl8125::MacVersion::rtl8125b ? "RTL8125B" : "RTL8125A",
        static_cast<unsigned>(record.final_status));
    return written >= 0 && std::ferror(output) == 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::puts("Usage: rtl8125_probe_collect SERIAL_LOG OUTPUT_JSON\n"
                  "Validates versioned read-only probe attempts; origin remains unverified.\n"
                  "Exit 0: record matches known Linux identity; 4: another supported identity.\n"
                  "Exit 2: arguments/I/O/input budget; 3: no complete valid probe attempt.");
        return 0;
    }

    if (argc != 3 || std::strcmp(argv[1], argv[2]) == 0) {
        std::fputs("Use --help; input and output must be different files.\n", stderr);
        return 2;
    }

    auto* input = std::fopen(argv[1], "rb");

    if (input == nullptr) {
        std::perror("Read serial log");
        return 2;
    }

    rtl8125::ProbeLogParser parser{};
    const auto valid = read_capture(input, parser);
    const auto close_status = std::fclose(input);

    if (!valid || close_status != 0) {
        std::fputs("Serial input failed or exceeded the 64 MiB budget.\n", stderr);
        return 2;
    }

    if (parser.completed_count() == 0) {
        std::fprintf(stderr, "No complete valid native probe attempt; discarded %zu.\n",
                     parser.rejected_count());
        return 3;
    }

    // Exclusive creation prevents overwriting existing evidence or aliased input
    // paths. No output file is created until the entire bounded input is checked.
    auto* output = std::fopen(argv[2], "wbx");

    if (output == nullptr) {
        std::perror("Create new evidence file");
        return 2;
    }

    const auto written = write_record(output, parser);
    const auto output_close_status = std::fclose(output);

    if (!written || output_close_status != 0) {
        std::fputs("Evidence write failed; retained partial output for inspection.\n", stderr);
        return 2;
    }

    std::puts("Captured unverified serial-log record; physical ReactOS acceptance requires boot provenance.");
    return rtl8125::matches_linux_card(parser.last_record()) ? 0 : 4;
}
