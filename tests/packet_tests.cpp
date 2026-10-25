// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.packet
Purpose: Boundary vectors for copied frames, Linux UDP/PTP quirks and filters.
Created: 2026-10-24
Architecture: ASan/UBSan portable tests; no kernel or network-peer evidence.
*/
#include "../src/core/packet.hpp"
#include "../src/core/filter.hpp"
#include "../src/core/frame_limits.hpp"
#include "../src/core/descriptor_bits.hpp"
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
unsigned checks{}, failures{};
void expect(bool value, const char* message)
{
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}
void udp(unsigned char* frame, bool ipv6, unsigned short port)
{
    std::memset(frame, 0x5a, 1516);
    frame[12] = ipv6 ? 0x86 : 0x08;
    frame[13] = ipv6 ? 0xdd : 0;
    frame[14] = ipv6 ? 0x60 : 0x45;
    frame[20] = ipv6 ? 17 : 0;
    frame[21] = 0;
    frame[23] = 17;
    const unsigned offset = ipv6 ? 54 : 34;
    frame[offset + 2] = static_cast<unsigned char>(port >> 8);
    frame[offset + 3] = static_cast<unsigned char>(port);
}
}
int main()
{
    using namespace rtl8125;
    unsigned char frame[1516]{};
    expect(pad_transmit_frame(nullptr, 60, 1514).status == PacketStatus::invalid_argument,
           "null storage rejected");
    for (std::size_t size = 0; size <= 1515; ++size) {
        std::memset(frame, 0x5a, sizeof(frame));
        const auto result = pad_transmit_frame(frame, size, 1514);
        const bool valid = size >= 14 && size <= 1514;
        expect((result.status == PacketStatus::ready) == valid, "Ethernet boundary size");
        if (valid) {
            const auto wanted = size < 60 ? 60 : size;
            expect(result.size == wanted && frame[wanted] == 0x5a, "bounded minimum padding");
            bool same = true;
            for (std::size_t index = 0; index < wanted; ++index) {
                same &= frame[index] == (index < size ? 0x5a : 0);
            }
            expect(same, "original bytes preserved and every pad byte zero");
        }
    }
    for (const bool ipv6 : {false, true}) {
        const std::size_t offset = ipv6 ? 54 : 34;
        for (const unsigned short port : {static_cast<unsigned short>(53),
                static_cast<unsigned short>(319), static_cast<unsigned short>(320)}) {
            for (std::size_t size = offset; size <= 175; ++size) {
                udp(frame, ipv6, port);
                auto wanted = size < 60 ? 60 : size;
                if (size < 175 && size - offset < 8 && offset + 8 > wanted) { wanted = offset + 8; }
                if (size < 175 && port != 53 && size - offset >= 4 && size - offset < 47) {
                    if (offset + 47 > wanted) { wanted = offset + 47; }
                }
                const auto result = pad_transmit_frame(frame, size, 1514);
                expect(result.status == PacketStatus::ready && result.size == wanted,
                       "IPv4/IPv6 UDP/PTP exact boundary table");
                bool zero = true;
                for (auto index = size; index < wanted; ++index) { zero &= frame[index] == 0; }
                expect(zero && frame[wanted] == 0x5a, "UDP/PTP padding never reads or writes past result");
            }
        }
    }
    udp(frame, false, 319);
    frame[14] = 0x4f;
    expect(pad_transmit_frame(frame, 42, 1514).size == 60, "truncated IPv4 options not parsed as transport");
    udp(frame, false, 319);
    frame[14] = 0x44;
    expect(pad_transmit_frame(frame, 42, 1514).size == 60, "invalid IPv4 IHL is opaque");
    udp(frame, false, 319);
    frame[21] = 1;
    expect(pad_transmit_frame(frame, 42, 1514).size == 60, "noninitial fragment has no UDP port parse");
    udp(frame, true, 319);
    frame[20] = 0;
    expect(pad_transmit_frame(frame, 62, 1514).size == 62, "IPv6 extension is outside direct UDP quirk");
    udp(frame, false, 319);
    expect(pad_transmit_frame(frame, 42, 80).status == PacketStatus::invalid_length && frame[42] == 0x5a,
           "PTP pad capacity rejected before mutation");
    for (const unsigned char type : {0x81, 0x88}) {
        std::memset(frame, 0, sizeof(frame)); frame[12] = type; frame[13] = type == 0x81 ? 0 : 0xa8;
        expect(pad_transmit_frame(frame, 60, 1514).status == PacketStatus::unsupported_vlan,
               "VLAN/QinQ outside initial policy");
    }
    for (unsigned wire = 0; wire < 2049; ++wire) {
        const auto size = received_frame_size(descriptor_first | descriptor_last | wire, 2048);
        expect(size == (wire >= 18 && wire <= 1518 ? wire - 4 : 0), "RX length/FCS boundary table");
    }
    expect(received_frame_size(descriptor_owned | descriptor_first | descriptor_last | 64, 2048) == 0 &&
           received_frame_size(receive_error | descriptor_first | descriptor_last | 64, 2048) == 0 &&
           received_frame_size(descriptor_first | 64, 2048) == 0 &&
           received_frame_size(descriptor_first | descriptor_last | 64, 63) == 0,
           "RX ownership errors fragments capacity rejected");
    PacketFilter filter{};
    const unsigned char station[]{2, 1, 2, 3, 4, 5};
    const unsigned char multicast[]{1, 0, 0x5e, 0, 0, 1, 0x33, 0x33, 0, 0, 0, 1};
    expect(valid_station_address(station) && !valid_station_address(multicast) &&
           !valid_station_address(nullptr), "station must be actual nonzero unicast");
    unsigned char scalar[5]{};
    scalar[1] = 0x2f;
    expect(set_packet_filter(scalar + 1, 4, filter).status == InformationStatus::success,
           "unaligned supported NDIS filter accepted");
    scalar[1] = 0x10;
    expect(set_packet_filter(scalar + 1, 4, filter).status == InformationStatus::unsupported &&
           filter.flags == 0x2f, "unsupported source-routing rejected transactionally");
    expect(set_packet_filter(scalar, 3, filter).status == InformationStatus::invalid_length,
           "filter exact scalar size");
    filter.flags = filter_multicast;
    expect(set_multicast_list(multicast, 6, filter).status == InformationStatus::success,
           "one multicast accepted");
    auto registers = filter_registers(filter);
    expect(registers.accept == 4 && registers.mar0 == 0 && registers.mar4 == 0x80,
           "IPv4 multicast published vector CRC bit31 and byte swapped MAR order");
    expect(set_multicast_list(multicast + 6, 6, filter).status == InformationStatus::success,
           "IPv6 multicast accepted");
    registers = filter_registers(filter);
    expect(registers.mar0 == 0x40 && registers.mar4 == 0,
           "IPv6 multicast CRC bit62 lands in swapped low MAR word");
    expect(set_multicast_list(station, 6, filter).status == InformationStatus::invalid_value &&
           filter.multicast[0][0] == 0x33, "unicast list rejected unchanged");
    std::memset(frame, 0xff, 6);
    expect(set_multicast_list(frame, 6, filter).status == InformationStatus::invalid_value,
           "broadcast not accepted as multicast list entry");
    expect(set_multicast_list(multicast, 7, filter).status == InformationStatus::invalid_length &&
           set_multicast_list(nullptr, 6, filter).status == InformationStatus::invalid_buffer &&
           set_multicast_list(multicast, 198, filter).status == InformationStatus::invalid_length,
           "list alignment null and capacity bounds");
    expect(set_multicast_list(multicast, 12, filter).status == InformationStatus::success,
           "two multicast addresses accepted");
    for (unsigned flags = 0; flags < 64; ++flags) {
        if ((flags & ~supported_packet_filters) != 0) { continue; }
        filter.flags = flags;
        std::memcpy(frame, station, 6);
        expect(accepts_frame(filter, station, frame, 60) == ((flags & (1 | 0x20)) != 0), "directed filter table");
        frame[5] ^= 1;
        expect(accepts_frame(filter, station, frame, 60) == ((flags & 0x20) != 0), "foreign unicast filter table");
        std::memset(frame, 0xff, 6);
        expect(accepts_frame(filter, station, frame, 60) == ((flags & (8 | 0x20)) != 0), "broadcast filter table");
        std::memcpy(frame, multicast, 6);
        expect(accepts_frame(filter, station, frame, 60) == ((flags & (2 | 4 | 0x20)) != 0), "listed multicast table");
        frame[5] ^= 2;
        expect(accepts_frame(filter, station, frame, 60) == ((flags & (4 | 0x20)) != 0), "hash collision exact software filter");
    }
    expect(set_multicast_list(nullptr, 0, filter).status == InformationStatus::success &&
           filter.multicast_count == 0, "empty list clears accepted addresses");
    filter.flags = filter_all_multicast;
    registers = filter_registers(filter);
    expect(registers.accept == 4 && registers.mar0 == UINT32_MAX && registers.mar4 == UINT32_MAX,
           "all multicast registers");
    filter.flags = filter_promiscuous;
    expect(filter_registers(filter).accept == 15, "promiscuous accepts all destination classes");
    std::printf("%u packet/filter checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
