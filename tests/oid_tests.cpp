// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.oid
Purpose: Verify real OID buffer helper boundaries and unchanged state on failure.
Created: 2026-10-24
Architecture: Host tests use production helper functions; no fake OS headers.
*/
#include "oid.hpp"
#include "frame_limits.hpp"

#include <array>
#include <cstdio>
#include <limits>

namespace {

struct Checks {
    unsigned total{};
    unsigned failures{};

    void expect(bool condition, const char* message) noexcept
    {
        ++total;

        if (!condition) {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", message);
        }
    }
};

} // namespace

int main()
{
    using namespace rtl8125;
    Checks checks;
    const auto expect = [&checks](bool condition, const char* message) noexcept {
        checks.expect(condition, message);
    };
    constexpr std::array<std::uint8_t, 4> source{0x78, 0x56, 0x34, 0x12};

    for (std::size_t capacity = 0; capacity <= 5; ++capacity) {
        std::array<std::uint8_t, 7> output{0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
        const auto before = output;
        const auto result = copy_information(output.data() + 1, capacity,
                                              source.data(), source.size());
        expect(output.front() == 0xaa && output.back() == 0xaa,
               "Output guards survive every advertised capacity");

        if (capacity < source.size()) {
            expect(result.status == InformationStatus::short_buffer &&
                   result.transferred == 0 && result.needed == source.size(),
                   "Short query reports exact requirement with zero transferred");
            expect(output == before, "Short query does not partially copy output");
        } else {
            expect(result.status == InformationStatus::success &&
                   result.transferred == source.size() && result.needed == 0,
                   "Successful query byte accounting");
            expect(output[1] == source[0] && output[4] == source[3] && output[5] == 0xaa,
                   "Unaligned query touches only requested output bytes");
        }
    }

    expect(copy_information(nullptr, 0, nullptr, 0).status == InformationStatus::success,
           "Empty multicast query accepts a null buffer");
    expect(copy_information(nullptr, 4, source.data(), 4).status ==
           InformationStatus::invalid_buffer, "Nonempty null output is invalid");
    std::array<std::uint8_t, 4> output{};
    expect(copy_information(output.data(), 4, nullptr, 4).status ==
           InformationStatus::invalid_buffer, "Nonempty null source is invalid");

    // A nonzero high half proves that 64-bit queries cannot silently return
    // only a zero/low word. Legacy requests deliberately return the low word.
    constexpr std::array<std::uint8_t, 8> golden_counter{
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    for (std::size_t capacity = 0; capacity <= 10; ++capacity) {
        std::array<std::uint8_t, 12> bytes{};
        bytes.fill(0xcc);
        const auto result = copy_counter_information(bytes.data() + 1, capacity,
                                                      UINT64_C(0x8877665544332211));
        const auto transferred = capacity < 4 ? 0u : (capacity < 8 ? 4u : 8u);
        expect(result.status == (capacity < 4 ? InformationStatus::short_buffer :
                                               InformationStatus::success) &&
               result.transferred == transferred && result.needed == 8,
               "Counter requests negotiate legacy32/full64 with exact byte counts");
        bool exact_bytes = bytes.front() == 0xcc;

        for (std::size_t index = 1; index < bytes.size(); ++index) {
            const auto expected = index <= transferred ? golden_counter[index - 1] : 0xcc;
            exact_bytes = exact_bytes && bytes[index] == expected;
        }

        expect(exact_bytes, "Counter query writes only negotiated bytes and keeps guards");
    }

    expect(copy_counter_information(nullptr, 8, UINT64_MAX).status ==
           InformationStatus::invalid_buffer, "Null counter output is refused");
    std::array<std::uint8_t, 8> full_counter{};
    const auto maximum_counter = copy_counter_information(full_counter.data(),
        std::numeric_limits<std::size_t>::max(), UINT64_MAX);
    expect(maximum_counter.status == InformationStatus::success &&
           maximum_counter.transferred == 8 && maximum_counter.needed == 8 &&
           full_counter[0] == 0xff && full_counter[7] == 0xff,
           "Huge advertised capacity still writes only the full64 counter");

    for (std::size_t length = 0; length <= 8; ++length) {
        std::uint32_t value = UINT32_MAX;
        const auto result = read_information_u32(source.data(), length, value);
        expect(length == 4 ? (result.status == InformationStatus::success &&
                            value == 0x12345678 && result.transferred == 4) :
                           (result.status == InformationStatus::invalid_length &&
                            value == 0 && result.transferred == 0 && result.needed == 4),
               "Set input length is exact and never narrows or reads past input");
    }

    std::uint32_t value = UINT32_MAX;
    expect(read_information_u32(nullptr, 4, value).status ==
           InformationStatus::invalid_buffer && value == 0, "Null scalar input is refused");
    expect(read_information_u32(source.data(), std::numeric_limits<std::size_t>::max(),
                                value).status == InformationStatus::invalid_length,
           "Huge length cannot wrap into a scalar");

    for (const auto candidate : {0u, 1u, 1500u, 1501u, UINT32_MAX}) {
        const std::array<std::uint8_t, 5> unaligned{
            0xcc, static_cast<std::uint8_t>(candidate),
            static_cast<std::uint8_t>(candidate >> 8),
            static_cast<std::uint8_t>(candidate >> 16),
            static_cast<std::uint8_t>(candidate >> 24)};
        std::uint32_t filter = 0xfeed;
        const auto filter_result = set_inactive_packet_filter(unaligned.data() + 1, 4,
                                                              filter);
        expect(candidate == 0 ? (filter_result.status == InformationStatus::success &&
                                 filter == 0 && filter_result.transferred == 4) :
                                (filter_result.status == InformationStatus::unsupported &&
                                 filter == 0xfeed && filter_result.transferred == 0),
               "Nonzero filter cannot claim unimplemented hardware programming");
        std::uint32_t lookahead = 512;
        const auto lookahead_result = set_lookahead(unaligned.data() + 1, 4, lookahead);
        expect(candidate <= maximum_frame_size - ethernet_header_size ?
                   (lookahead_result.status == InformationStatus::success &&
                    lookahead == candidate && lookahead_result.transferred == 4) :
                   (lookahead_result.status == InformationStatus::invalid_value &&
                    lookahead == 512 && lookahead_result.transferred == 0),
               "Lookahead accepts only bounded MTU values and preserves state on failure");
    }

    std::uint32_t filter = 17;
    expect(set_inactive_packet_filter(source.data(), 3, filter).status ==
           InformationStatus::invalid_length && filter == 17,
           "Truncated filter leaves adapter state unchanged");
    std::uint32_t lookahead = 512;
    expect(set_lookahead(nullptr, 4, lookahead).status ==
           InformationStatus::invalid_buffer && lookahead == 512,
           "Null lookahead leaves adapter state unchanged");
    std::printf("OID boundary checks: %u passed, %u failed\n",
                checks.total - checks.failures, checks.failures);
    return checks.failures == 0 ? 0 : 1;
}
