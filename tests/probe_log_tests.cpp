// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.probe_log
Purpose: Verify bounded capture parsing, consistency and attempt isolation.
Created: 2026-10-24
Architecture: Synthetic serial strings only; none are physical execution evidence.
*/
#include "probe_log.hpp"

#include <cstdio>
#include <cstring>

namespace {

unsigned checks{};
unsigned failures{};
constexpr const char* begin = "RTL8125: probe begin v1";
constexpr const char* resources = "RTL8125: rev 05 BAR2 address 00000000:df800000 length 00010000";
constexpr const char* interrupt = "RTL8125: IRQ vector 53 level 5 flags 0000 TxConfig 64100000";
constexpr const char* identity = "RTL8125: probe XID 641, MAC family 2; datapath incomplete";
constexpr const char* end = "RTL8125: probe end status c00000bb";

void expect(bool condition, const char* message) noexcept
{
    ++checks;

    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void feed(rtl8125::ProbeLogParser& parser, const char* line) noexcept
{
    parser.consume_line(line, std::strlen(line));
}

void valid_attempt(rtl8125::ProbeLogParser& parser) noexcept
{
    feed(parser, begin);
    feed(parser, resources);
    feed(parser, interrupt);
    feed(parser, identity);
    feed(parser, end);
}

void test_valid() noexcept
{
    rtl8125::ProbeLogParser parser{};
    feed(parser, "unrelated boot diagnostic");
    valid_attempt(parser);
    parser.finish();
    const auto& record = parser.last_record();
    expect(parser.completed_count() == 1 && parser.rejected_count() == 0, "one complete marked attempt");
    expect(record.revision == 5 && record.bar_index == 2 && record.address == 0xdf800000 &&
           record.length == 0x10000 && record.interrupt_vector == 53 && record.interrupt_level == 5 &&
           record.interrupt_flags == 0 && record.tx_config == 0x64100000 &&
           record.chip.xid == 0x641 && record.chip.version == rtl8125::MacVersion::rtl8125b &&
           record.final_status == 0xc00000bb, "all fields agree with production chip/resource rules");
    expect(rtl8125::matches_linux_card(record), "matching stable Linux identity fields");

    valid_attempt(parser);
    feed(parser, begin);
    feed(parser, "RTL8125: rev 05 BAR2 address 12345678:df800000 length 00010000\r");
    feed(parser, interrupt);
    feed(parser, identity);
    feed(parser, end);
    expect(parser.completed_count() == 3 && parser.last_record().address == 0x12345678df800000ULL,
           "last complete attempt preserves 64-bit address and CRLF line ending");

    rtl8125::ProbeLogParser a{};
    feed(a, begin);
    feed(a, "RTL8125: rev 04 BAR2 address 00000001:23450000 length 00010000");
    feed(a, "RTL8125: IRQ vector 53 level 5 flags 0001 TxConfig 60900000");
    feed(a, "RTL8125: probe XID 609, MAC family 1; datapath incomplete");
    feed(a, end);
    expect(a.completed_count() == 1 && a.last_record().chip.version == rtl8125::MacVersion::rtl8125a &&
           !rtl8125::matches_linux_card(a.last_record()), "other supported family is valid but mismatch reported");
}

void test_boundaries() noexcept
{
    const char* lines[]{begin, resources, interrupt, identity, end};

    for (unsigned stage = 0; stage < 5; ++stage) {
        for (std::size_t cut = 0; cut < std::strlen(lines[stage]); ++cut) {
            rtl8125::ProbeLogParser parser{};

            for (unsigned index = 0; index < 5; ++index) {
                parser.consume_line(lines[index], index == stage ? cut : std::strlen(lines[index]));
            }

            parser.finish();
            expect(parser.completed_count() == 0, "every incomplete diagnostic line rejected");
        }
    }

    char text[rtl8125::maximum_probe_line_size + 1]{};
    std::memset(text, 'x', sizeof(text));
    rtl8125::ProbeLogParser parser{};
    feed(parser, begin);
    parser.consume_line(text, sizeof(text));
    expect(parser.rejected_count() == 1 && parser.completed_count() == 0, "oversized line rejected before read");
    feed(parser, begin);
    parser.consume_line(nullptr, 100);
    expect(parser.rejected_count() == 2, "null line discards pending attempt");
    feed(parser, begin);
    char embedded_nul[]{'x', '\0', 'y'};
    parser.consume_line(embedded_nul, sizeof(embedded_nul));
    expect(parser.rejected_count() == 3, "embedded NUL rejected");
    feed(parser, begin);
    feed(parser, "RTL8125: rev 05\nBAR2 address 00000000:df800000 length 00010000");
    expect(parser.rejected_count() == 4, "embedded newline rejected");
}

void test_malformed() noexcept
{
    const char* bad_resources[]{
        "RTL8125: rev 100 BAR2 address 00000000:df800000 length 00010000",
        "RTL8125: rev 05 BAR6 address 00000000:df800000 length 00010000",
        "RTL8125: rev 05 BAR-1 address 00000000:df800000 length 00010000",
        "RTL8125: rev 05 BAR2 address 00000000:00000000 length 00010000",
        "RTL8125: rev 05 BAR2 address 00000000:df800001 length 00010000",
        "RTL8125: rev 05 BAR2 address ffffffff:fffffff0 length 00010000",
        "RTL8125: rev 05 BAR2 address 100000000:df800000 length 00010000",
        "RTL8125: rev 05 BAR2 address 00000000:df800000 length 00000100",
        "RTL8125: rev 05 BAR2 address 00000000:df800000 length 0000604b",
        "RTL8125: rev 05 BAR2 address 00000000:df800000 length 100000000",
        "RTL8125: rev 05 BAR2 address 00000000:df800000 length 00010000 extra"
    };

    for (const auto bad : bad_resources) {
        rtl8125::ProbeLogParser parser{};
        feed(parser, begin);
        feed(parser, bad);
        feed(parser, interrupt);
        feed(parser, identity);
        feed(parser, end);
        expect(parser.completed_count() == 0 && parser.rejected_count() == 1,
               "resource width/range/syntax violation rejected");
    }

    const char* bad_interrupts[]{
        "RTL8125: IRQ vector 0 level 5 flags 0000 TxConfig 64100000",
        "RTL8125: IRQ vector 53 level 0 flags 0000 TxConfig 64100000",
        "RTL8125: IRQ vector 53 level 5 flags 0002 TxConfig 64100000",
        "RTL8125: IRQ vector 4294967296 level 5 flags 0000 TxConfig 64100000",
        "RTL8125: IRQ vector 53 level 5 flags 0000 TxConfig ffffffff",
        "RTL8125: IRQ vector 53 level 5 flags 0000 TxConfig 64900000",
        "RTL8125: IRQ vector 53 level 5 flags 0000 TxConfig 00000000"
    };

    for (const auto bad : bad_interrupts) {
        rtl8125::ProbeLogParser parser{};
        feed(parser, begin);
        feed(parser, resources);
        feed(parser, bad);
        feed(parser, identity);
        feed(parser, end);
        expect(parser.completed_count() == 0 && parser.rejected_count() == 1,
               "interrupt/XID width and unsupported device rejected");
    }

    const char* bad_identities[]{
        "RTL8125: probe XID 609, MAC family 2; datapath incomplete",
        "RTL8125: probe XID 641, MAC family 1; datapath incomplete",
        "RTL8125: probe XID 641, MAC family 2; datapath ready"
    };

    for (const auto bad : bad_identities) {
        rtl8125::ProbeLogParser parser{};
        feed(parser, begin);
        feed(parser, resources);
        feed(parser, interrupt);
        feed(parser, bad);
        feed(parser, end);
        expect(parser.completed_count() == 0, "identity must agree with TxConfig and incomplete status");
    }

    rtl8125::ProbeLogParser successful_init{};
    feed(successful_init, begin);
    feed(successful_init, resources);
    feed(successful_init, interrupt);
    feed(successful_init, identity);
    feed(successful_init, "RTL8125: probe end status 00000000");
    expect(successful_init.completed_count() == 0, "unexpected initialize success never accepted");
}

void test_attempt_isolation() noexcept
{
    rtl8125::ProbeLogParser parser{};
    feed(parser, resources);
    feed(parser, interrupt);
    feed(parser, identity);
    feed(parser, end);
    expect(parser.completed_count() == 0, "orphan unmarked triples never accepted");
    feed(parser, begin);
    feed(parser, resources);
    feed(parser, begin);
    feed(parser, interrupt);
    feed(parser, identity);
    feed(parser, end);
    expect(parser.completed_count() == 0 && parser.rejected_count() == 2,
           "new begin cannot combine old BAR with later IRQ/XID");
    feed(parser, begin);
    feed(parser, resources);
    feed(parser, "another kernel diagnostic");
    feed(parser, interrupt);
    feed(parser, identity);
    feed(parser, end);
    expect(parser.completed_count() == 0 && parser.rejected_count() == 3,
           "interrupted partial record discarded instead of joining later fields");
    valid_attempt(parser);
    const auto previous_address = parser.last_record().address;
    feed(parser, begin);
    feed(parser, "RTL8125: rev 05 BAR2 address 00000001:12340000 length 00010000");
    feed(parser, interrupt);
    parser.finish();
    expect(parser.completed_count() == 1 && parser.rejected_count() == 4 &&
           parser.last_record().address == previous_address,
           "EOF discards partial attempt without corrupting last complete record");
    parser.finish();
    expect(parser.rejected_count() == 4, "finalization is idempotent");
}

} // namespace

int main()
{
    test_valid();
    test_boundaries();
    test_malformed();
    test_attempt_isolation();
    std::printf("RTL8125 probe-log parser: %u checks, %u failures (synthetic text only)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
