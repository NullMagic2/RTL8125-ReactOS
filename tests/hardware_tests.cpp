// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.hardware
Purpose: Model indirect registers, firmware control flow and PHY failure budgets.
Created: 2026-10-24
Architecture: Host-only model evidence; never PCI, MMIO or physical execution.
*/
#include "../src/hardware/phy.hpp"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
using namespace rtl8125::hardware;
unsigned checks{};
unsigned failures{};
void expect(bool condition, const char* message) noexcept
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
struct Model {
    std::array<std::uint16_t, 32768> mac{};
    std::array<std::uint16_t, 32768> phy{};
    std::array<std::uint16_t, 32> ephy{};
    std::array<std::uint32_t, 1024> eri{};
    std::uint32_t mac_latch{}, phy_latch{}, ephy_latch{}, eri_latch{}, eri_data{};
    std::uint64_t now{};
    unsigned accesses{}, fail_at{}, writes{}, claims{}, releases{};
    bool locked{}, stuck{}, removed{};
};
bool claim(void* context) noexcept
{
    auto& model = *static_cast<Model*>(context);
    ++model.claims;
    if (model.locked) { return false; }
    model.locked = true;
    return true;
}
void release(void* context) noexcept
{
    auto& model = *static_cast<Model*>(context);
    model.locked = false;
    ++model.releases;
}
bool read(void* context, std::uint32_t offset, unsigned width, std::uint32_t& value) noexcept
{
    auto& model = *static_cast<Model*>(context);
    ++model.accesses;
    value = 0;
    if (model.fail_at == model.accesses || width != 4) { return false; }
    if (model.removed) { value = UINT32_MAX; return true; }
    switch (offset) {
    case 0xb0: value = model.mac_latch; break;
    case 0xb8: value = model.phy_latch; break;
    case 0x80: value = model.ephy_latch; break;
    case 0x74: value = model.eri_latch; break;
    case 0x70: value = model.eri_data; break;
    default: return false;
    }
    return true;
}
bool write(void* context, std::uint32_t offset, unsigned width, std::uint32_t value) noexcept
{
    auto& model = *static_cast<Model*>(context);
    ++model.accesses;
    if (model.fail_at == model.accesses || width != 4) { return false; }
    ++model.writes;
    const auto index = (value >> 16) & 0x7fff;
    const auto is_write = (value & 0x80000000) != 0;
    switch (offset) {
    case 0xb0:
        if (is_write) { model.mac[index] = static_cast<std::uint16_t>(value); }
        model.mac_latch = model.mac[index];
        break;
    case 0xb8:
        if (is_write) { model.phy[index] = static_cast<std::uint16_t>(value); }
        model.phy_latch = model.phy[index] | ((is_write != model.stuck) ? 0 : 0x80000000);
        break;
    case 0x80:
        if (is_write) { model.ephy[index & 0x1f] = static_cast<std::uint16_t>(value); }
        model.ephy_latch = model.ephy[index & 0x1f] | ((is_write != model.stuck) ? 0 : 0x80000000);
        break;
    case 0x70: model.eri_data = value; break;
    case 0x74:
        if (is_write) {
            const auto mask = (value >> 12) & 0xf;
            auto& destination = model.eri[(value & 0xfff) / 4];
            for (unsigned byte = 0; byte < 4; ++byte) {
                if ((mask & (1u << byte)) != 0) {
                    const auto byte_mask = UINT32_C(0xff) << (byte * 8);
                    destination = (destination & ~byte_mask) | (model.eri_data & byte_mask);
                }
            }
        } else { model.eri_data = model.eri[(value & 0xfff) / 4]; }
        model.eri_latch = (is_write != model.stuck) ? 0 : 0x80000000;
        break;
    default: return false;
    }
    return true;
}
bool delay(void* context, std::uint32_t value) noexcept
{
    static_cast<Model*>(context)->now += value;
    return true;
}
std::uint64_t now(void* context) noexcept { return static_cast<Model*>(context)->now; }
RegisterIo callbacks(Model& model) noexcept { return {&model, claim, release, read, write, delay, now}; }

void put32(std::uint8_t* bytes, std::uint32_t value) noexcept
{
    for (unsigned index = 0; index < 4; ++index) { bytes[index] = static_cast<std::uint8_t>(value >> (index * 8)); }
}
template<std::size_t Count>
std::array<std::uint8_t, Count * 4> code(const std::array<std::uint32_t, Count>& actions) noexcept
{
    std::array<std::uint8_t, Count * 4> bytes{};
    for (std::size_t index = 0; index < Count; ++index) { put32(bytes.data() + index * 4, actions[index]); }
    return bytes;
}
void test_registers() noexcept
{
    Model model{};
    {
        RegisterSession io(callbacks(model));
        RegisterSession concurrent(callbacks(model));
        expect(concurrent.status() == Status::busy, "overlapping sequence rejected");
        std::uint16_t word{};
        std::uint32_t dword{};
        expect(io.mac_write(0xe040, 0x1234) && io.mac_read(0xe040, word) && word == 0x1234,
               "MAC OCP write/read encoding");
        expect(io.mac_modify(0xe040, 0x00f0, 0x0005) && model.mac[0xe040 / 2] == 0x1205,
               "MAC read/modify/write retains unrelated bits");
        expect(io.phy_write(0xa400, 0x1357) && io.phy_read(0xa400, word) && word == 0x1357,
               "PHY read/write completion polarity");
        expect(io.mdio_write(0x1f, 0xa43) && io.mdio_write(0x13, 0x2468) &&
               model.phy[0xa436 / 2] == 0x2468 && io.mdio_read(0x1f, word) && word == 0xa43,
               "paged MDIO has Linux translated OCP address");
        expect(io.mdio_write(0x1f, 0) && io.mdio_read(0, word) && word == 0x1357,
               "zero MDIO page restores standard PHY base");
        expect(io.mcu_write(0x1f, 0xe00) && io.mcu_write(0x40, 0x5678) &&
               io.mcu_read(0x40, word) && word == 0x5678, "firmware MCU uses shared OCP base");
        expect(io.ephy_write(0x62, 0x9abc) && io.ephy_read(0x22, word) && word == 0x9abc,
               "EPHY masks banks exactly as Linux primitive");
        expect(io.eri_write(0x1a0, 5, 0x12345678) && io.eri_read(0x1a0, dword) && dword == 0x00340078,
               "ERI byte masks and command order");
    }
    expect(!model.locked && model.releases == 1, "scope releases only successful claim");
    for (unsigned mode = 0; mode < 7; ++mode) {
        Model failure{};
        failure.stuck = mode == 0;
        failure.removed = mode == 1;
        failure.fail_at = mode == 2 ? 2 : 0;
        AccessBudget budget{};
        if (mode == 3) { budget.operations = 1; }
        if (mode == 4) { budget.delay_us = 1; }
        if (mode == 5) { budget.elapsed_us = 1; }
        RegisterSession io(callbacks(failure), budget);
        std::uint16_t value = 99;
        expect(!(mode == 6 ? io.phy_read(0x10000, value) : io.phy_read(0xa400, value)) && value == 0,
               "invalid, removed, timeout, I/O and budget errors clear result");
        const auto calls = failure.accesses;
        expect(!io.phy_write(0xa400, 1) && failure.accesses == calls, "first error prevents later hardware calls");
    }
    for (unsigned fault = 1; fault <= 3; ++fault) {
        Model failure{};
        failure.fail_at = fault;
        RegisterSession io(callbacks(failure));
        expect(!io.mac_modify(0xe040, 0xff, 1), "MAC transaction failure latches");
    }
}

void test_firmware() noexcept
{
    FirmwareView view{};
    expect(validate_firmware(nullptr, 4, view) == FirmwareStatus::invalid_format, "null firmware rejected");
    auto bytes = code<15>({0x801f0000, 0x80010012, 0x00010000, 0x10000080, 0x200000ff,
        0xc0020000, 0x90000001, 0x8003ffff, 0xa0010092, 0x8004ffff, 0xb0011234,
        0x8005ffff, 0x70000000, 0xe0000001, 0x800600ab});
    expect(validate_firmware(bytes.data(), bytes.size(), view) == FirmwareStatus::ok, "all conditional Linux opcodes validate");
    Model model{};
    {
        RegisterSession io(callbacks(model));
        const auto result = execute_firmware(view, io);
        expect(result.status == FirmwareStatus::ok && result.steps == 12 && result.instruction == 15,
               "firmware read count and comparison skips preserve upstream flow");
        expect(model.phy[0xa404 / 2] == 0x92 && model.phy[0xa406 / 2] == 0 &&
               model.phy[0xa408 / 2] == 0 && model.phy[0xa40a / 2] == 0 && model.phy[0xa40c / 2] == 0xab,
               "firmware OR/AND/previous writes exact expected registers");
    }
    auto mcu = code<5>({0x40000001, 0x801f0e00, 0x80401234, 0x00400000, 0xc0420000});
    validate_firmware(mcu.data(), mcu.size(), view);
    {
        RegisterSession io(callbacks(model));
        expect(execute_firmware(view, io).status == FirmwareStatus::ok && model.mac[0xe042 / 2] == 0x1234,
               "MDIO_CHG selects MCU callbacks and shared base");
    }
    auto loop = code<2>({0x70000000, 0x30000000});
    validate_firmware(loop.data(), loop.size(), view);
    {
        RegisterSession io(callbacks(model));
        expect(execute_firmware(view, io, 20).status == FirmwareStatus::budget_exhausted,
               "firmware backward loop terminates on step budget");
    }
    auto delay_code = code<1>({0xe000ffff});
    validate_firmware(delay_code.data(), delay_code.size(), view);
    {
        RegisterSession io(callbacks(model));
        expect(execute_firmware(view, io).status == FirmwareStatus::budget_exhausted,
               "large firmware delay rejected before sleep");
    }
    std::array<std::uint8_t, 52> header{};
    std::memcpy(header.data() + 4, "fixture-v1", 10);
    put32(header.data() + 36, 48);
    put32(header.data() + 40, 1);
    put32(header.data() + 48, 0x80010023);
    std::uint8_t checksum{};
    for (auto byte : header) { checksum = static_cast<std::uint8_t>(checksum + byte); }
    header[44] = static_cast<std::uint8_t>(0 - checksum);
    expect(validate_firmware(header.data(), header.size(), view) == FirmwareStatus::ok &&
           std::strcmp(view.version, "fixture-v1") == 0, "header format checksum and version");
    for (std::size_t size = 0; size < header.size(); ++size) {
        expect(validate_firmware(header.data(), size, view) != FirmwareStatus::ok && view.bytes == nullptr,
               "all truncated header extents reject and clear output");
    }
    header[51] ^= 1;
    expect(validate_firmware(header.data(), header.size(), view) == FirmwareStatus::checksum,
           "header corruption fails checksum");
    for (const auto action : {0x50000000u, 0x60000000u, 0xf0000000u, 0x40000002u,
                              0x30010000u, 0xd0010000u, 0x90000001u}) {
        auto invalid = code<1>({action});
        expect(validate_firmware(invalid.data(), invalid.size(), view) != FirmwareStatus::ok,
               "reserved opcode, mode and out-of-bounds branches rejected");
        Model empty{};
        RegisterSession io(callbacks(empty));
        FirmwareView malicious{invalid.data(), invalid.size(), 0, 1, {}};
        expect(execute_firmware(malicious, io).status != FirmwareStatus::ok && empty.writes == 0,
               "execution revalidates forged view before device writes");
    }
    std::uint32_t random = 0x8125;
    std::array<std::uint8_t, 128> fuzz{};
    for (unsigned iteration = 0; iteration < 2000; ++iteration) {
        for (auto& byte : fuzz) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            byte = static_cast<std::uint8_t>(random);
        }
        const auto size = iteration % fuzz.size();
        const auto result = validate_firmware(fuzz.data(), size, view);
        expect(result != FirmwareStatus::ok || (view.instruction_count <= size / 4 && view.bytes == fuzz.data()),
               "deterministic arbitrary firmware bytes remain bounded");
    }
}

void test_phy() noexcept
{
    auto bytes = code<1>({0x801f0000});
    FirmwareView view{};
    validate_firmware(bytes.data(), bytes.size(), view);
    Model model{};
    {
        RegisterSession io(callbacks(model));
        expect(configure_ephy(io, rtl8125::MacVersion::rtl8125b) && model.ephy[0xb] == 0xa908 &&
               model.ephy[0x1e] == 0x20eb && model.ephy[2] == 0x20, "B EPHY source table and masked RMW");
        expect(configure_rtl8125b_phy(io, view), "complete B PHY sub-sequence succeeds in device model");
        expect(model.phy[0xa442 / 2] == 0x0800 && model.phy[0xac46 / 2] == 0x90 &&
               model.phy[0xad30 / 2] == 1 && model.phy[0xa436 / 2] == 0x807a &&
               model.phy[0xa438 / 2] == 0x2417 && model.phy[0xa4ca / 2] == 0x40,
               "B PHY final tuning values and ten parameter writes");
    }
    const auto operations = model.accesses;
    for (unsigned fail_at = 1; fail_at <= operations; ++fail_at) {
        Model failed{};
        failed.fail_at = fail_at;
        {
            RegisterSession io(callbacks(failed));
            const auto success = configure_ephy(io, rtl8125::MacVersion::rtl8125b) &&
                configure_rtl8125b_phy(io, view);
            expect(!success && io.status() == Status::io_error, "each native I/O failure aborts B sequence");
            const auto calls = failed.accesses;
            expect(!io.mac_write(0xe040, 1) && calls == failed.accesses,
                   "partial PHY failure prevents subsequent hardware mutation");
        }
        expect(!failed.locked && failed.releases == 1, "failed sequence releases exclusive software owner");
    }
    Model reset{};
    reset.phy[0xa400 / 2] = 0x8000;
    RegisterSession io(callbacks(reset));
    expect(!configure_rtl8125b_phy(io, view) && io.status() == Status::timeout && reset.now >= 600000,
           "firmware soft reset cannot hang the initialization worker");
}
}

int main()
{
    test_registers(); test_firmware(); test_phy();
    std::printf("RTL8125 hardware/firmware models: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
