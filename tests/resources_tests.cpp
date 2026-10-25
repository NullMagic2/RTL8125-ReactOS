// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.resources
Purpose: Exercise PCI selection, assigned-list truncation and MMIO boundaries.
Created: 2026-10-24
Architecture: Host tests production core over byte snapshots, never hardware.
*/
#include "resources.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

unsigned checks{};
unsigned failures{};

void expect(bool condition, const char* message) noexcept
{
    ++checks;

    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void put32(std::uint8_t* bytes, std::uint32_t value) noexcept
{
    for (unsigned index = 0; index < 4; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

void put64(std::uint8_t* bytes, std::uint64_t value) noexcept
{
    put32(bytes, static_cast<std::uint32_t>(value));
    put32(bytes + 4, static_cast<std::uint32_t>(value >> 32));
}

void config_fixture(std::uint8_t* bytes) noexcept
{
    std::memset(bytes, 0, 64);
    put32(bytes, 0x812510ec);
    bytes[4] = 2;
    bytes[8] = 5;
    bytes[11] = 2;
    put32(bytes + 16, 0x1001); // Port BAR precedes 64-bit memory BAR.
    put32(bytes + 20, 0x23450004);
    put32(bytes + 24, 1);
}

std::size_t assigned_fixture(std::uint8_t* bytes, std::size_t stride) noexcept
{
    std::memset(bytes, 0, 128);
    bytes[0] = 1;
    bytes[2] = 1;
    put32(bytes + 4, 3);
    auto* port = bytes + 8;
    port[0] = 1;
    put64(port + 4, 0x1000);
    put32(port + 12, 256);
    auto* memory = port + stride;
    memory[0] = 3;
    put64(memory + 4, UINT64_C(0x123450000));
    put32(memory + 12, 0x10000);
    auto* interrupt = memory + stride;
    interrupt[0] = 2;
    interrupt[1] = 3;
    put32(interrupt + 4, 5);
    put32(interrupt + 8, 0x35);
    put32(interrupt + 12, 1);

    if (stride == 20) {
        put32(interrupt + 16, 0x80000000);
    }

    return 8 + 3 * stride;
}

void check_pci() noexcept
{
    using namespace rtl8125;
    std::uint8_t config[64]{};
    config_fixture(config);
    PciRegisterBar bar{};
    expect(decode_register_bar(config, sizeof(config), bar), "valid 64-bit first memory BAR");
    expect(bar.address == UINT64_C(0x123450000) && bar.index == 1 &&
           bar.revision == 5 && bar.is_64_bit, "BAR address high bits and identity preserved");

    for (std::size_t size = 0; size < sizeof(config); ++size) {
        bar.address = 1;
        expect(!decode_register_bar(config, size, bar) && bar.address == 0,
               "every truncated PCI config clears output");
    }

    expect(!decode_register_bar(nullptr, 64, bar), "null PCI snapshot rejected");
    const unsigned malformed_offsets[]{0, 2, 4, 10, 11, 14};

    for (const auto offset : malformed_offsets) {
        config_fixture(config);
        config[offset] ^= 2;
        expect(!decode_register_bar(config, sizeof(config), bar), "wrong PCI identity/type/decode rejected");
    }

    config_fixture(config);
    put32(config + 20, 0x23450002);
    expect(!decode_register_bar(config, 64, bar), "unsupported 20-bit BAR encoding rejected");
    put32(config + 20, 0x23450008);
    expect(!decode_register_bar(config, 64, bar), "prefetchable control BAR rejected");
    config_fixture(config);
    put32(config + 20, 0x23450000);
    expect(decode_register_bar(config, 64, bar) && !bar.is_64_bit && bar.address == 0x23450000,
           "32-bit memory BAR accepted");
    std::memset(config + 16, 0, 24);
    put32(config + 36, 4);
    expect(!decode_register_bar(config, 64, bar), "64-bit BAR cannot begin at final slot");
    put32(config + 36, 0);
    expect(!decode_register_bar(config, 64, bar), "missing memory BAR rejected");

    // Read-only 64-byte PCI config from the actual card, captured 2026-10-24
    // in evidence/m02-linux-device-probe.txt. This tests the real BAR ordering;
    // it is not a ReactOS execution or a hardware TxConfig observation.
    constexpr std::uint8_t actual_pci_config[64]{
        0xec, 0x10, 0x25, 0x81, 0x07, 0x04, 0x10, 0x00,
        0x05, 0x00, 0x00, 0x02, 0x10, 0x00, 0x00, 0x00,
        0x01, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x80, 0xdf, 0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x81, 0xdf, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x62, 0x14, 0x27, 0x7e,
        0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0a, 0x01, 0x00, 0x00
    };
    expect(decode_register_bar(actual_pci_config, sizeof(actual_pci_config), bar) &&
           bar.index == 2 && bar.address == 0xdf800000 && bar.is_64_bit && bar.revision == 5,
           "actual Linux PCI snapshot selects 64-bit BAR2, not short BAR4");
}

void check_assigned() noexcept
{
    using namespace rtl8125;
    const PciRegisterBar bar{UINT64_C(0x123450000), 1, 5, true};
    std::uint8_t bytes[128]{};
    AssignedResources resources{};

    for (const std::size_t stride : {std::size_t{16}, std::size_t{20}}) {
        const auto size = assigned_fixture(bytes, stride);
        expect(decode_assigned_resources(bytes, size, stride, bar, resources), "valid native CM list");
        expect(resources.address == bar.address && resources.length == 0x10000 &&
               resources.interrupt_vector == 0x35 && resources.interrupt_level == 5 &&
               resources.interrupt_shared && resources.interrupt_affinity ==
               (stride == 20 ? UINT64_C(0x8000000000000001) : 1),
               "assigned memory and native affinity preserved");

        for (std::size_t length = 0; length < size; ++length) {
            resources.address = 7;
            expect(!decode_assigned_resources(bytes, length, stride, bar, resources) &&
                   resources.address == 0, "all assigned-list truncations clear output");
        }

        expect(!decode_assigned_resources(bytes, size + 1, stride, bar, resources), "unexpected tail rejected");
        put32(bytes + 4, UINT32_MAX);
        expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "count multiplication bounded");

        const auto memory_offset = 8 + stride;
        const auto irq_offset = memory_offset + stride;

        for (const auto aperture : {UINT32_C(0), UINT32_C(256), minimum_register_aperture - 1}) {
            assigned_fixture(bytes, stride);
            put32(bytes + memory_offset + 12, aperture);
            expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "short memory aperture rejected");
        }

        assigned_fixture(bytes, stride);
        put32(bytes + memory_offset + 12, minimum_register_aperture);
        expect(decode_assigned_resources(bytes, size, stride, bar, resources), "exact minimum aperture accepted");

        for (const auto flag : {1U, 2U, 4U, 8U, 32U, 512U}) {
            assigned_fixture(bytes, stride);
            bytes[memory_offset + 2] = static_cast<std::uint8_t>(flag);
            bytes[memory_offset + 3] = static_cast<std::uint8_t>(flag >> 8);
            expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "unsafe memory flags rejected");
        }

        assigned_fixture(bytes, stride);
        bytes[memory_offset + 2] = 128;
        expect(decode_assigned_resources(bytes, size, stride, bar, resources), "BAR annotation allowed");
        bytes[memory_offset] = 1;
        expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "port cannot satisfy memory BAR");
        assigned_fixture(bytes, stride);
        put64(bytes + memory_offset + 4, bar.address - 16);
        expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "encompassing unrelated range rejected");
        put64(bytes + memory_offset + 4, bar.address + 16);
        expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "mismatched assigned address rejected");
        assigned_fixture(bytes, stride);
        std::memcpy(bytes + 8, bytes + memory_offset, stride);
        expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "duplicate assigned BAR rejected");

        for (const auto overlap_offset : {UINT64_C(16), UINT64_C(0xffff)}) {
            assigned_fixture(bytes, stride);
            bytes[8] = 3;
            put64(bytes + 12, bar.address + overlap_offset);
            put32(bytes + 20, 256);
            expect(!decode_assigned_resources(bytes, size, stride, bar, resources) &&
                   resources.address == 0, "partially overlapping assigned memory is ambiguous");
        }

        assigned_fixture(bytes, stride);
        bytes[8] = 3;
        put64(bytes + 12, bar.address + 0x10000);
        put32(bytes + 20, 256);
        expect(decode_assigned_resources(bytes, size, stride, bar, resources),
               "adjacent assigned memory does not overlap the register aperture");
        assigned_fixture(bytes, stride);
        std::memcpy(bytes + 8, bytes + irq_offset, stride);
        expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "duplicate interrupt rejected");

        for (const auto offset : {2U, 4U, 8U, 12U}) {
            assigned_fixture(bytes, stride);
            put32(bytes + irq_offset + offset, offset == 2 ? 2 : 0);

            if (offset == 12 && stride == 20) {
                put32(bytes + irq_offset + 16, 0);
            }

            expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "invalid IRQ or MSI rejected");
        }

        assigned_fixture(bytes, stride);
        bytes[8] = 5;
        expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "variable device-specific tail rejected");
        bytes[8] = 7;
        expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "unsupported large-memory format rejected");
        assigned_fixture(bytes, stride);
        put64(bytes + 12, UINT64_MAX - 2);
        bytes[8] = 3;
        expect(!decode_assigned_resources(bytes, size, stride, bar, resources), "physical interval overflow rejected");
    }

    expect(!decode_assigned_resources(nullptr, 8, 16, bar, resources), "null CM bytes rejected");
    expect(!decode_assigned_resources(bytes, 8, 24, bar, resources), "invalid CM stride rejected");
    expect(!decode_assigned_resources(bytes, maximum_resource_list_size + 1, 16, bar, resources),
           "resource-size budget checked before memory access");
}

void check_mmio() noexcept
{
    using namespace rtl8125;

    for (const auto width : {1U, 2U, 4U}) {
        expect(valid_mmio_access(0x10000, 0x10000 - width, width), "last complete register accepted");
        expect(!valid_mmio_access(0x10000, 0x10000, width), "one-past aperture rejected");
        expect(!valid_mmio_access(UINT32_MAX, UINT32_MAX, width), "offset overflow rejected");
        expect(!valid_mmio_access(0, 0, width), "empty aperture rejected");

        for (unsigned offset = 0; offset < 12; ++offset) {
            expect(valid_mmio_access(8, offset, width) ==
                   (offset <= 8 - width && offset % width == 0), "alignment and tail bounds enforced");
        }
    }

    expect(!valid_mmio_access(100, 0, 0) && !valid_mmio_access(100, 0, 3) &&
           !valid_mmio_access(100, 0, 8), "unsupported MMIO widths rejected");
}

} // namespace

int main()
{
    check_pci();
    check_assigned();
    check_mmio();
    std::printf("RTL8125 resources: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
