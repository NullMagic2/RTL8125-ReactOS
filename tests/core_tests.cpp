// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.core
Purpose: Verify upstream vectors and malformed descriptor boundaries.
Created: 2026-10-24
Architecture: Host-only executable over production core, no device access.
*/
#include "chip.hpp"
#include "descriptor.hpp"

#include <cstdio>
#include <cstring>
#include <limits>

namespace {

struct Checks {
    unsigned total{};
    unsigned failed{};

    void expect(bool condition, const char* description) noexcept
    {
        ++total;

        if (!condition) {
            ++failed;
            std::fprintf(stderr, "FAIL: %s\n", description);
        }
    }
};

void check_identity(Checks& checks) noexcept
{
    using namespace rtl8125;
    // Golden XIDs from the pinned Linux family table. Bit 31 of TxConfig is
    // outside the Linux family mask; lower TxConfig bits are unrelated fields.
    const auto a = identify_chip(0x10ec, 0x8125, 0x60900000);
    const auto b = identify_chip(0x10ec, 0x8125, 0x64100000);
    const auto b_alias = identify_chip(0x10ec, 0x8125, 0xe41fffff);
    checks.expect(a.status == ChipStatus::supported && a.version == MacVersion::rtl8125a,
                  "Linux XID 609 is RTL8125A");
    checks.expect(b.status == ChipStatus::supported && b.version == MacVersion::rtl8125b,
                  "Linux XID 641 is RTL8125B");
    checks.expect(b_alias.status == ChipStatus::supported &&
                  b_alias.version == MacVersion::rtl8125b, "XID mask ignores variant bits");
    checks.expect(a.xid == 0x609 && b.xid == 0x641, "XID extraction");
    checks.expect(identify_chip(0x10ec, 0x8125, UINT32_MAX).status ==
                  ChipStatus::inaccessible, "Removed device read is refused");
    checks.expect(identify_chip(0x10ec, 0x8125, 0).status == ChipStatus::unknown_mac,
                  "Zero MMIO snapshot does not select a MAC");
    checks.expect(identify_chip(0x10ec, 0x8125, 0x60800000).status == ChipStatus::unknown_mac,
                  "Withdrawn Linux XID 608 is refused");
    checks.expect(identify_chip(0x10ec, 0x8125, 0x64900000).status == ChipStatus::unknown_mac,
                  "RTL8126 XID is refused even with a misleading PCI ID");
    checks.expect(identify_chip(0x10ec, 0x8168, 0x64100000).status == ChipStatus::wrong_device,
                  "RTL8168 is outside the selected port");
    checks.expect(identify_chip(0x8086, 0x8125, 0x64100000).status == ChipStatus::wrong_device,
                  "Foreign vendor is refused");
    checks.expect(std::strcmp(firmware_name(a.version), "rtl_nic/rtl8125a-3.fw") == 0,
                  "A firmware dependency");
    checks.expect(std::strcmp(firmware_name(b.version), "rtl_nic/rtl8125b-2.fw") == 0,
                  "B firmware dependency");
    checks.expect(firmware_name(MacVersion::unsupported) == nullptr,
                  "Unknown MAC has no firmware fallback");
}

void check_descriptors(Checks& checks) noexcept
{
    using namespace rtl8125;
    // Linux legacy format: options1=FS|LS|EOR|60, options2=0, 64-bit LE bus address.
    constexpr std::array<std::uint8_t, 16> golden_tx{
        0x3c, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    const auto tx = prepare_tx({UINT64_C(0x1122334455667788)}, 60, 255, 256);
    checks.expect(tx.status == DescriptorStatus::ready && tx.image.bytes == golden_tx,
                  "Byte-exact final TX descriptor, full 64-bit DMA address, OWN clear");
    const auto first = prepare_tx({0}, 1514, 0, 256);
    checks.expect(first.status == DescriptorStatus::ready && first.image.bytes[0] == 0xea &&
                  first.image.bytes[1] == 0x05 && first.image.bytes[3] == 0x30,
                  "Maximum frame has no EOR except in final slot");
    const auto rx = prepare_rx({0x1000}, 2048, 0, 1);
    constexpr std::array<std::uint8_t, 16> golden_rx{
        0x00, 0x08, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    checks.expect(rx.status == DescriptorStatus::ready && rx.image.bytes == golden_rx,
                  "Byte-exact prepared RX descriptor, no ownership publication");
    checks.expect(prepare_tx({0}, 60, 0, 0).status == DescriptorStatus::invalid_ring,
                  "Empty ring refused");
    checks.expect(prepare_tx({0}, 60, 256, 256).status == DescriptorStatus::invalid_ring,
                  "Out-of-range descriptor index refused");
    checks.expect(prepare_tx({0}, 59, 0, 1).status == DescriptorStatus::invalid_size,
                  "TX caller must pad short frames");
    checks.expect(prepare_tx({0}, 1515, 0, 1).status == DescriptorStatus::invalid_size,
                  "VLAN and jumbo TX are outside initial contract");
    checks.expect(prepare_rx({0}, 1517, 0, 1).status == DescriptorStatus::invalid_size,
                  "RX buffer must fit complete frame and FCS");
    checks.expect(prepare_rx({0}, 0x4000, 0, 1).status == DescriptorStatus::invalid_size,
                  "RX length may not overflow descriptor field");
    checks.expect(prepare_tx({UINT64_MAX - 58}, 60, 0, 1).status ==
                  DescriptorStatus::address_overflow, "DMA interval overflow refused");
    checks.expect(prepare_tx({UINT64_MAX - 59}, 60, 0, 1).status == DescriptorStatus::ready,
                  "DMA interval ending at UINT64_MAX is representable");
    checks.expect(prepare_rx({0}, std::numeric_limits<std::size_t>::max(), 0, 1).status ==
                  DescriptorStatus::invalid_size, "Oversized host length cannot narrow");
    checks.expect(prepare_tx({0}, 60, std::numeric_limits<std::size_t>::max(), 1).status ==
                  DescriptorStatus::invalid_ring, "Huge index cannot wrap");
    const auto invalid = prepare_tx({0}, 0, 0, 1);
    checks.expect(invalid.image.bytes == DescriptorImage{}.bytes,
                  "Failure contains no stale descriptor bytes");
}

void check_receive(Checks& checks) noexcept
{
    using namespace rtl8125;
    constexpr auto frame_flags = descriptor_first | descriptor_last;
    const auto packet = decode_receive(frame_flags | 64, 2048);
    checks.expect(packet.status == ReceiveStatus::ready && packet.frame_size == 60,
                  "Receive strips FCS exactly once");
    const auto max_frame = decode_receive(frame_flags | 1518, 1518);
    checks.expect(max_frame.status == ReceiveStatus::ready && max_frame.frame_size == 1514,
                  "Maximum untagged receive fits exact DMA capacity");
    checks.expect(decode_receive(frame_flags | 64 | descriptor_owned, 2048).status ==
                  ReceiveStatus::hardware_owned, "Live hardware-owned descriptor is refused");
    checks.expect(decode_receive(frame_flags | 64 | receive_error, 2048).status ==
                  ReceiveStatus::device_error, "RX error summary refuses damaged packet");
    checks.expect(decode_receive(descriptor_first | 64, 2048).status ==
                  ReceiveStatus::fragmented, "Missing final fragment refused");
    checks.expect(decode_receive(descriptor_last | 64, 2048).status ==
                  ReceiveStatus::fragmented, "Missing initial fragment refused");
    checks.expect(decode_receive(frame_flags | 1518, 1517).status ==
                  ReceiveStatus::invalid_size, "Completion cannot exceed allocated DMA buffer");
    checks.expect(decode_receive(frame_flags | 1519, 2048).status ==
                  ReceiveStatus::invalid_size, "VLAN/jumbo receive refused by initial policy");
    checks.expect(decode_receive(frame_flags | 64, 0).status == ReceiveStatus::invalid_size,
                  "Completion with no backing buffer refused");

    for (std::uint32_t size = 0; size < 18; ++size) {
        const auto short_frame = decode_receive(frame_flags | size, 2048);
        checks.expect(short_frame.status == ReceiveStatus::invalid_size &&
                      short_frame.frame_size == 0, "Short completion cannot underflow FCS");
    }

    for (std::uint32_t size = 18; size <= 1518; ++size) {
        const auto result = decode_receive(frame_flags | size, 2048);
        checks.expect(result.status == ReceiveStatus::ready && result.frame_size + 4 == size,
                      "Every accepted length remains within buffer and preserves payload");
    }
}

} // namespace

int main()
{
    Checks checks{};
    check_identity(checks);
    check_descriptors(checks);
    check_receive(checks);
    std::printf("RTL8125 core: %u checks, %u failures\n", checks.total, checks.failed);
    return checks.failed == 0 ? 0 : 1;
}
