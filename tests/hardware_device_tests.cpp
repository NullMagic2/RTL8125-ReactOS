// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.hardware_device
Purpose: Verify complete initialization order, link truth and failure containment.
Created: 2026-10-24
Architecture: Host-only register/PCI model. No real device or physical DMA.
*/
#include "../src/hardware/device.hpp"
#ifdef RTL8125_NATIVE_BRIDGE
#pragma push_macro("__in")
#undef __in
#endif
#include <array>
#ifdef RTL8125_NATIVE_BRIDGE
#pragma pop_macro("__in")
#endif
#include <cstdio>
#include <initializer_list>
#ifdef RTL8125_NATIVE_BRIDGE
#include "../src/reactos/hardware_init.hpp"
#endif

namespace {
using namespace rtl8125;
using namespace rtl8125::hardware;
unsigned checks{}, failures{};
void expect(bool success, const char* description) noexcept
{
    ++checks;
    if (!success) { ++failures; std::printf("FAIL: %s\n", description); }
}
struct Event { std::uint32_t offset{}, value{}; unsigned width{}; };
struct Model {
    std::array<std::uint8_t, 65536> mmio{};
    std::array<std::uint8_t, 4096> pci{};
    std::array<std::uint16_t, 32768> mac{}, phy{};
    std::array<std::uint16_t, 32> ephy{};
    std::array<Event, 8192> events{};
    std::uint32_t mac_latch{}, phy_latch{}, ephy_latch{}, csi_latch{}, csi_data{};
    std::uint64_t time{};
    unsigned accesses{}, fail_at{}, event_count{}, releases{}, reset_count{};
    bool claimed{}, extended{}, stuck_reset{}, stuck_phy{}, stuck_fifo{}, removed{};
};
void put(std::uint8_t* destination, std::uint32_t value, unsigned width) noexcept
{
    for (unsigned index = 0; index < width; ++index) { destination[index] = static_cast<std::uint8_t>(value >> (index * 8)); }
}
std::uint32_t get(const std::uint8_t* source, unsigned width) noexcept
{
    std::uint32_t value{};
    for (unsigned index = 0; index < width; ++index) { value |= std::uint32_t(source[index]) << (index * 8); }
    return value;
}
void setup(Model& model) noexcept
{
    put(model.pci.data(), 0x812510ec, 4);
    model.pci[6] = 0x10;
    model.pci[0x34] = 0x40;
    model.pci[0x40] = 0x10;
    put(model.pci.data() + 0x48, 0x1234, 2);
    put(model.pci.data() + 0x50, 0x0103, 2);
    model.pci[0x70c] = 0x35;
    put(model.mmio.data() + 0x19e0, 0x33221102, 4);
    put(model.mmio.data() + 0x19e4, 0x5544, 2);
    model.mmio[0xd3] = 0x32;
    put(model.mmio.data() + 0xe2, 0x0103, 2);
    model.mac[0xeb58 / 2] = 0xffff;
    model.phy[0xa404 / 2] = 0x001c;
    model.phy[0xa406 / 2] = 0xc840;
    model.phy[0xa616 / 2] = 0x2000;
    model.phy[0xa402 / 2] = 0x7908;
    model.phy[0xa41e / 2] = 0x2000;
}
bool claim(void* context) noexcept
{
    auto& m = *static_cast<Model*>(context);
    if (m.claimed) { return false; }
    m.claimed = true;
    return true;
}
void release(void* context) noexcept
{
    auto& m = *static_cast<Model*>(context);
    m.claimed = false;
    ++m.releases;
}
bool available(Model& m) noexcept { return ++m.accesses != m.fail_at; }
bool read(void* context, std::uint32_t offset, unsigned width, std::uint32_t& value) noexcept
{
    auto& m = *static_cast<Model*>(context);
    value = 0;
    if (!available(m) || offset > m.mmio.size() - width) { return false; }
    if (m.removed) { value = width == 1 ? 0xffu : width == 2 ? 0xffffu : UINT32_MAX; return true; }
    if (width == 4) {
        if (offset == 0xb0) { value = m.mac_latch; return true; }
        if (offset == 0xb8) { value = m.phy_latch; return true; }
        if (offset == 0x80) { value = m.ephy_latch; return true; }
        if (offset == 0x68) { value = m.csi_latch; return true; }
        if (offset == 0x64) { value = m.csi_data; return true; }
    }
    value = get(m.mmio.data() + offset, width);
    if (m.stuck_fifo && offset == 0xd3) { value = 0; }
    return true;
}
bool write(void* context, std::uint32_t offset, unsigned width, std::uint32_t value) noexcept
{
    auto& m = *static_cast<Model*>(context);
    if (!available(m) || offset > m.mmio.size() - width) { return false; }
    if (m.event_count < m.events.size()) { m.events[m.event_count++] = {offset, value, width}; }
    const auto index = (value >> 16) & 0x7fff;
    const bool writing = (value & 0x80000000) != 0;
    if (width == 4) {
        if (offset == 0xb0) {
            if (writing) { m.mac[index] = static_cast<std::uint16_t>(value); }
            m.mac_latch = m.mac[index]; return true;
        }
        if (offset == 0xb8) {
            if (writing) {
                auto data = static_cast<std::uint16_t>(value);
                if (index == 0xa400 / 2 && (data & 0x8000) != 0) {
                    ++m.reset_count;
                    if (!m.stuck_phy) { data &= ~0x8000; }
                }
                m.phy[index] = data;
            }
            m.phy_latch = m.phy[index] | (writing ? 0u : 0x80000000u); return true;
        }
        if (offset == 0x80) {
            if (writing) { m.ephy[index & 31] = static_cast<std::uint16_t>(value); }
            m.ephy_latch = m.ephy[index & 31] | (writing ? 0u : 0x80000000u); return true;
        }
        if (offset == 0x64) { m.csi_data = value; return true; }
        if (offset == 0x68) {
            const auto address = value & 0xffc;
            if (writing) { put(m.pci.data() + address, m.csi_data, 4); }
            else { m.csi_data = get(m.pci.data() + address, 4); }
            m.csi_latch = writing ? 0u : 0x80000000u; return true;
        }
    }
    put(m.mmio.data() + offset, value, width);
    if (offset == 0x37 && (value & 0x10) != 0 && !m.stuck_reset) { m.mmio[0x37] = 0; }
    return true;
}
bool pci_read(void* context, std::uint32_t offset, unsigned width, std::uint32_t& value) noexcept
{
    auto& m = *static_cast<Model*>(context);
    value = 0;
    if (!available(m) || offset > m.pci.size() - width || (offset >= 256 && !m.extended)) { return false; }
    value = get(m.pci.data() + offset, width);
    return true;
}
bool pci_write(void* context, std::uint32_t offset, unsigned width, std::uint32_t value) noexcept
{
    auto& m = *static_cast<Model*>(context);
    if (!available(m) || offset > m.pci.size() - width || (offset >= 256 && !m.extended)) { return false; }
    put(m.pci.data() + offset, value, width);
    return true;
}
bool delay(void* context, std::uint32_t value) noexcept { static_cast<Model*>(context)->time += value; return true; }
std::uint64_t now(void* context) noexcept { return static_cast<Model*>(context)->time; }
RegisterIo callbacks(Model& model) noexcept { return {&model, claim, release, read, write, delay, now, pci_read, pci_write, 0}; }
FirmwareView firmware() noexcept
{
    static constexpr std::uint8_t bytes[] = {0, 0, 0x1f, 0x80};
    FirmwareView view{};
    validate_firmware(bytes, sizeof(bytes), view);
    return view;
}
unsigned event_position(const Model& m, std::uint32_t offset, std::uint32_t value) noexcept
{
    for (unsigned index = 0; index < m.event_count; ++index) {
        if (m.events[index].offset == offset && m.events[index].value == value) { return index; }
    }
    return 99999;
}
void test_initialize(MacVersion version) noexcept
{
    Model model{};
    setup(model);
    DeviceState state{};
    {
        RegisterSession io(callbacks(model));
        expect(initialize_device(io, version, firmware(), state), "full A/B initialization succeeds");
    }
    expect(state.initialized && !state.running && !state.recovery_required && state.version == version,
           "initialized state does not claim running networking");
    expect(model.mmio[0x37] == 0 && get(model.mmio.data() + 0x38, 4) == 0,
           "initialization leaves IRQs masked and engines stopped");
    expect(state.permanent_address[0] == 2 && state.permanent_address[5] == 0x55 &&
           get(model.mmio.data(), 4) == 0x33221102, "real backup station address programmed");
    expect((model.mac[0xeb58 / 2] & 1) == 0 && model.mac[0xe614 / 2] ==
           (version == MacVersion::rtl8125a ? 0x300 : 0x200), "legacy descriptor and exact A/B MAC quirks");
    expect(get(model.pci.data() + 0x50, 2) == 0 && get(model.pci.data() + 0x48, 2) == 0x5234 &&
           get(model.pci.data() + 0x70c, 4) == 0x27000035, "PCIe policy and CSI latency preserve unrelated bits");
    expect(model.phy[0xa408 / 2] == 0x01e1 && model.phy[0xa412 / 2] == 0x0200 &&
           (model.phy[0xa5d4 / 2] & 0x180) == 0x80 && model.phy[0xa5d0 / 2] == 0 &&
           model.phy[0xa6d4 / 2] == 0, "10/100/1000/2500 advertised with EEE and pause disabled");
    expect(model.reset_count == 1 && model.releases == 1 && !model.claimed,
           "post tuning PHY reset and software exclusion release");
    expect(event_position(model, 0x37, 0x10) < event_position(model, 0x382, 0x221b) &&
           event_position(model, 0x382, 0x221b) < event_position(model, 0xb0, 0x75ac0000),
           "reset precedes MAC start and legacy descriptor selection");
    if (version == MacVersion::rtl8125a) {
        constexpr std::uint16_t curve[] = {0x43, 0xa7, 0xd6, 0xec, 0xf6, 0xfb, 0xfd, 0xff,
            0xbb, 0x58, 0x29, 0x13, 9, 4, 2};
        const auto start = event_position(model, 0xb8, 0xd21b81b3);
        expect(start < model.event_count, "A coefficient selector occurs before firmware");
        for (unsigned index = 0; index < 40; ++index) {
            const auto expected = index < 15 ? curve[index] : 0;
            expect(start + index + 1 < model.event_count &&
                model.events[start + index + 1].offset == 0xb8 &&
                model.events[start + index + 1].value == (0xd21c0000u | expected),
                "A coefficient data preserves all fifteen values then twenty-five zeros");
        }
    }
    const auto initialization_operations = model.accesses;
    put(model.mmio.data() + 0x44, get(model.mmio.data() + 0x44, 4) | 0x0e, 4);
    RingConfiguration rings{0x100000, 0x200000, 2048};
    {
        RegisterSession io(callbacks(model));
        expect(activate_device(io, state, rings) && state.running, "explicit activation starts validated rings");
    }
    expect(event_position(model, 0x24, 0) < event_position(model, 0x20, 0x100000) &&
           event_position(model, 0xe8, 0) < event_position(model, 0xe4, 0x200000), "ring high halves programmed before low halves");
    expect(model.mmio[0x37] == 0x0c && get(model.mmio.data() + 0x38, 4) == 0 &&
           get(model.mmio.data() + 0xda, 2) == maximum_frame_size + ethernet_fcs_size + 1,
           "activation keeps IRQs masked and honors the MTU1500 receive limit");
    expect((get(model.mmio.data() + 0x44, 4) & 0x3f) == 0x0e,
           "activation preserves M06 directed/broadcast/multicast acceptance bits");
    const auto activation_operations = model.accesses - initialization_operations;
    // Inject failure at every basic PCI/MMIO operation of complete initialization.
    // A rejected extended PCI write may validly recover using the CSI fallback.
    for (unsigned index = 1; index <= initialization_operations; ++index) {
        Model failed{}; setup(failed); failed.fail_at = index;
        DeviceState failed_state{};
        {
            RegisterSession io(callbacks(failed));
            const auto success = initialize_device(io, version, firmware(), failed_state);
            if (!success) {
                expect(!failed_state.initialized && !failed_state.running,
                       "each I/O failure keeps device unusable");
                const auto accesses = failed.accesses;
                expect(!io.write(0x37, 1, 0x0c) && failed.accesses == accesses,
                       "sticky failure prevents unsafe continuation");
            } else {
                expect(failed_state.initialized && !failed_state.running,
                       "extended PCI failure falls back without claiming active engines");
            }
        }
        if (failed_state.recovery_required) {
            RegisterSession cleanup(callbacks(failed));
            expect(contain_device(cleanup, failed_state) && failed_state.recovery_required,
                   "fresh recovery scope contains without claiming DMA retirement");
        }
        expect(!failed.claimed && failed.releases >= 1, "all failed initialization scopes release exclusion");
    }
    for (unsigned index = 1; index <= activation_operations; ++index) {
        Model failed{}; setup(failed); failed.fail_at = index;
        DeviceState prepared{}; prepared.initialized = true; prepared.version = version;
        {
            RegisterSession io(callbacks(failed));
            expect(!activate_device(io, prepared, rings) && prepared.recovery_required &&
                   !prepared.initialized && !prepared.running,
                   "every ring activation failure revokes readiness without claiming retirement");
        }
        RegisterSession cleanup(callbacks(failed));
        expect(contain_device(cleanup, prepared) && (failed.mmio[0x37] & 0x0c) == 0 &&
               get(failed.mmio.data() + 0x38, 4) == 0,
               "failed activation receives independent engine/IRQ containment");
    }

}
void test_failures() noexcept
{
    for (unsigned mode = 0; mode < 12; ++mode) {
        Model model{}; setup(model); DeviceState state{};
        if (mode == 0) { model.stuck_fifo = true; }
        if (mode == 1) { model.stuck_reset = true; }
        if (mode == 2) { model.stuck_phy = true; }
        if (mode == 3) { model.pci[0x40] = 1; model.pci[0x41] = 0x40; }
        if (mode == 4) { put(model.mmio.data() + 0x19e0, 0, 4); put(model.mmio.data() + 0x19e4, 0, 2); }
        if (mode == 5) { model.mac[0xe00e / 2] = 0x2000; }
        if (mode == 6) { model.removed = true; }
        if (mode == 8) { model.phy[0xa406 / 2] = 0xdead; }
        if (mode == 9) { model.phy[0xa616 / 2] = 0; }
        if (mode == 10) { model.phy[0xa402 / 2] = 0; }
        if (mode == 11) { model.phy[0xa41e / 2] = 0; }
        AccessBudget budget{};
        if (mode == 7) { budget.operations = 10; }
        RegisterSession io(callbacks(model), budget);
        expect(!initialize_device(io, MacVersion::rtl8125b, firmware(), state) &&
               !state.initialized && !state.running && state.recovery_required,
               "timeouts, invalid PCI/MAC, removal and budget exhaustion cannot establish readiness");
    }
    for (unsigned mode = 0; mode < 9; ++mode) {
        Model model{}; setup(model); DeviceState state{};
        RegisterSession io(callbacks(model));
        const auto before = model.accesses;
        RingConfiguration rings{0x100000, 0x200000, 2048};
        if (mode != 0) { state.initialized = true; state.version = MacVersion::rtl8125b; }
        if (mode == 1) { rings.transmit = 0xfffff100; }
        if (mode == 2) { rings.receive = 0; }
        if (mode == 3) { rings.receive_buffer_size = 1517; }
        if (mode == 4) { rings.receive = 0x200001; }
        if (mode == 5) { state.recovery_required = true; }
        if (mode == 6) { rings.receive = rings.transmit + 256; }
        if (mode == 7) { state.version = MacVersion::unsupported; }
        if (mode == 8) { rings.receive_buffer_size = 4096; }
        expect(!activate_device(io, state, rings) && model.accesses == before,
               "invalid state or DMA ranges reject before any device write");
    }
    Model wrong_card{}; setup(wrong_card); wrong_card.pci[0] = 0x99;
    DeviceState wrong_state{};
    {
        RegisterSession unknown(callbacks(wrong_card));
        expect(!initialize_device(unknown, MacVersion::rtl8125b, firmware(), wrong_state) &&
               !wrong_state.recovery_required && wrong_card.event_count == 0 && wrong_card.accesses == 1,
               "changed PCI identity rejects before writes and cannot request containment on an unknown device");
    }
    Model extended{}; setup(extended); extended.extended = true;
    DeviceState state{};
    RegisterSession io(callbacks(extended));
    expect(initialize_device(io, MacVersion::rtl8125a, firmware(), state) &&
           extended.pci[0x70f] == 0x27 && extended.csi_latch == 0,
           "native PCI extended config succeeds without touching CSI");
}
void test_quiesce() noexcept
{
    for (const auto version : {MacVersion::rtl8125a, MacVersion::rtl8125b}) {
        Model model{}; setup(model);
        put(model.mmio.data() + 0x44, 0xdeadbe3f, 4);
        model.mmio[0x37] = 0x0c;
        {
            RegisterSession io(callbacks(model));
            expect(quiesce_device(io, version), "A/B partial-start-safe quiesce completes");
        }
        expect(get(model.mmio.data() + 0x44, 4) == 0xdeadbe00 &&
               (get(model.mmio.data() + 0xf0, 4) & 0x80000) != 0 && model.mmio[0x37] == 0,
               "drain rejects RX and gates ingress before stopping engines");
        expect(event_position(model, 0x38, 0) < event_position(model, 0x44, 0xdeadbe00) &&
               event_position(model, 0xf0, 0x80000) < event_position(model, 0x37, 0x10) &&
               model.time >= 4200 && model.releases == 1,
               "interrupt mask, ingress close, settle and reset follow pinned cleanup order");
        expect((event_position(model, 0x37, 0x8c) != 99999) == (version == MacVersion::rtl8125b),
               "StopReq is required only for RTL8125B FIFO drain");
        for (unsigned fault = 0; fault < 4; ++fault) {
            Model failed{}; setup(failed);
            if (fault == 0) { failed.stuck_fifo = true; }
            if (fault == 1) { failed.stuck_reset = true; }
            if (fault == 2) { failed.removed = true; }
            if (fault == 3) { put(failed.mmio.data() + 0xe2, 0, 2); }
            RegisterSession io(callbacks(failed));
            expect(quiesce_device(io, version) == (fault == 3 && version == MacVersion::rtl8125a),
                   "bounded FIFO/reset/removal and variant-specific secondary drain failures reject");
        }
        for (unsigned fail = 1; fail < model.accesses; ++fail) {
            Model failed{}; setup(failed); failed.fail_at = fail;
            RegisterSession io(callbacks(failed));
            expect(!quiesce_device(io, version), "every injected quiesce I/O failure remains uncertain");
        }
    }
    Model unsupported{}; setup(unsupported);
    RegisterSession io(callbacks(unsupported));
    expect(!quiesce_device(io, MacVersion::unsupported) && unsupported.accesses == 0,
           "unknown MAC rejects before any quiesce access");
}

void test_link() noexcept
{
    constexpr std::uint16_t codes[] = {0, 0x10, 0x20, 0x210};
    constexpr std::uint32_t speeds[] = {10, 100, 1000, 2500};
    for (unsigned index = 0; index < 4; ++index) {
        Model model{}; setup(model);
        model.phy[0xa402 / 2] = 0x24; model.phy[0xa400 / 2] = 0x1000;
        model.phy[0xa434 / 2] = codes[index]; model.mmio[0x6c] = 3;
        DeviceState state{}; state.initialized = true; state.version = MacVersion::rtl8125b;
        LinkState link{true, 9999, true, true};
        RegisterSession io(callbacks(model));
        expect(read_link(io, state, link) && link.up && link.autoneg_complete &&
               link.full_duplex && link.speed_mbps == speeds[index], "actual downshift status selects every supported rate");
    }
    for (unsigned mode = 0; mode < 8; ++mode) {
        Model model{}; setup(model);
        model.phy[0xa402 / 2] = 0x24; model.phy[0xa400 / 2] = 0x1000;
        model.phy[0xa434 / 2] = 0x210; model.mmio[0x6c] = 3;
        DeviceState state{}; state.initialized = true; state.version = MacVersion::rtl8125b;
        if (mode == 0) { model.phy[0xa402 / 2] = 0x20; }
        if (mode == 1) { model.phy[0xa402 / 2] = 4; }
        if (mode == 2) { model.phy[0xa400 / 2] = 0; }
        if (mode == 3) { model.mmio[0x6c] = 0; }
        if (mode == 4) { model.phy[0xa434 / 2] = 0x220; }
        if (mode == 5) { model.mmio[0x6c] = 2; }
        if (mode == 6) { model.removed = true; }
        if (mode == 7) { state.recovery_required = true; }
        RegisterSession io(callbacks(model)); LinkState link{true, 9999, true, true};
        read_link(io, state, link);
        expect(!link.up && link.speed_mbps == 0 && !link.full_duplex,
               "down, incomplete negotiation, invalid speed/duplex and removal never leak stale link state");
    }
}
#ifdef RTL8125_NATIVE_BRIDGE
Model* native_model{};
KIRQL native_irql{};
bool native_timing_valid{true};
unsigned native_stalls{}, native_sleeps{};
KIRQL NTAPI mock_irql() { return native_irql; }
ULONGLONG NTAPI mock_time() { return native_model->time * 10; }
VOID NTAPI mock_sleep(ULONG interval)
{
    native_timing_valid = native_timing_valid && interval > 100;
    ++native_sleeps;
    native_model->time += interval;
}
VOID NTAPI mock_stall(ULONG interval)
{
    native_timing_valid = native_timing_valid && interval > 0 && interval <= 50;
    ++native_stalls;
    native_model->time += interval;
}
ULONG NTAPI mock_pci_read(NDIS_HANDLE handle, ULONG slot, ULONG offset, PVOID destination, ULONG size)
{
    expect(handle == reinterpret_cast<NDIS_HANDLE>(0x1234) && slot == 0,
           "native PCI reads retain actual adapter ownership");
    std::uint32_t value{};
    if (!pci_read(native_model, offset, size, value)) { return 0; }
    put(static_cast<std::uint8_t*>(destination), value, size);
    return size;
}
ULONG NTAPI mock_pci_write(NDIS_HANDLE handle, ULONG slot, ULONG offset, PVOID source, ULONG size)
{
    expect(handle == reinterpret_cast<NDIS_HANDLE>(0x1234) && slot == 0,
           "native PCI writes retain actual adapter ownership");
    return pci_write(native_model, offset, size, get(static_cast<const std::uint8_t*>(source), size)) ? size : 0;
}
NTSTATUS NTAPI mock_create(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                           PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG)
{ return STATUS_UNSUCCESSFUL; }
NTSTATUS NTAPI mock_query(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS)
{ return STATUS_UNSUCCESSFUL; }
NTSTATUS NTAPI mock_file_read(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK,
                              PVOID, ULONG, PLARGE_INTEGER, PULONG)
{ return STATUS_UNSUCCESSFUL; }
NTSTATUS NTAPI mock_close(HANDLE) { return STATUS_SUCCESS; }
PVOID NTAPI mock_allocate(POOL_TYPE, SIZE_T, ULONG) { return nullptr; }
VOID NTAPI mock_free(PVOID, ULONG) {}
void test_native() noexcept
{
    using namespace rtl8125::reactos;
    Model model{};
    native_model = &model;
    setup(model);
    DeviceResources resources{};
    resources.mapped_address = model.mmio.data();
    resources.mapped_length = static_cast<ULONG>(model.mmio.size());
    resources.chip = {ChipStatus::supported, MacVersion::rtl8125b, 0x641};
    HardwareAccess access{};
    access.resources = &resources;
    access.adapter_handle = reinterpret_cast<NDIS_HANDLE>(0x1234);
    const auto view = firmware();
    FirmwareBlob blob{};
    blob.bytes = const_cast<PUCHAR>(view.bytes);
    blob.size = static_cast<ULONG>(view.byte_count);
    blob.view = view;
    blob.version = MacVersion::rtl8125b;
    HardwareState state{};
    native_irql = DISPATCH_LEVEL;
    expect(initialize_hardware(access, blob, state) == NDIS_STATUS_INVALID_DATA && model.accesses == 0,
           "actual native initialization rejects wrong IRQL before any I/O");
    native_irql = PASSIVE_LEVEL;
    access.claimed = 1;
    expect(initialize_hardware(access, blob, state) != NDIS_STATUS_SUCCESS && model.accesses == 0,
           "actual native exclusive claim refuses overlap");
    access.claimed = 0;
    blob.version = MacVersion::rtl8125a;
    expect(initialize_hardware(access, blob, state) == NDIS_STATUS_INVALID_DATA && model.accesses == 0,
           "actual native firmware identity must match the assigned card");
    blob.version = MacVersion::rtl8125b;
    access.pci_function = 8;
    expect(initialize_hardware(access, blob, state) == NDIS_STATUS_INVALID_DATA && model.accesses == 0,
           "native CSI function must be a real three-bit PCI function");
    access.pci_function = 0;
    expect(initialize_hardware(access, blob, state) == NDIS_STATUS_SUCCESS &&
           state.device.initialized && !state.device.running && access.claimed == 0,
           "genuine SDK bridge completes production MAC/PHY code with model I/O");
    expect(native_timing_valid && native_stalls != 0 && native_sleeps != 0,
           "native short polls use bounded processor stalls while long waits yield");
    model.phy[0xa402 / 2] = 0x24; model.phy[0xa434 / 2] = 0x210; model.mmio[0x6c] = 3;
    LinkState link{};
    expect(query_hardware_link(access, state, link) == NDIS_STATUS_SUCCESS &&
           link.up && link.speed_mbps == 2500, "native link API returns actual modeled PHY speed");
    put(model.mmio.data() + 0x44, get(model.mmio.data() + 0x44, 4) | 0x0e, 4);
    expect(activate_hardware(access, state, {0x100000, 0x200000, 2048}) == NDIS_STATUS_SUCCESS &&
           state.device.running && get(model.mmio.data() + 0x38, 4) == 0,
           "native explicit activation leaves interrupt publication to M07");
    expect((get(model.mmio.data() + 0x44, 4) & 0x3f) == 0x0e &&
           get(model.mmio.data() + 0xda, 2) == maximum_frame_size + ethernet_fcs_size + 1,
           "native activation retains M06 filters and MTU limit independently of DMA capacity");
    native_irql = DISPATCH_LEVEL;
    link = {true, 2500, true, true};
    expect(query_hardware_link(access, state, link) == NDIS_STATUS_INVALID_DATA &&
           !link.up && link.speed_mbps == 0, "native wrong-IRQL link error clears stale output");
    native_irql = PASSIVE_LEVEL;
    state = {};
    model.fail_at = model.accesses + 40;
    expect(initialize_hardware(access, blob, state) != NDIS_STATUS_SUCCESS &&
           state.device.recovery_required && !state.device.initialized &&
           !state.device.running && access.claimed == 0 && (model.mmio[0x37] & 0x0c) == 0,
           "native failed sequence releases claim before independent containment");
}
#endif

}
int main()
{
#ifdef RTL8125_NATIVE_BRIDGE
    test_native();
#else
    test_initialize(MacVersion::rtl8125a); test_initialize(MacVersion::rtl8125b);
    test_failures(); test_link(); test_quiesce();
#endif
    std::printf("RTL8125 complete hardware models: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

#ifdef RTL8125_NATIVE_BRIDGE
namespace rtl8125::reactos {
bool read_mmio8(const DeviceResources&, ULONG offset, UCHAR& value) noexcept
{
    std::uint32_t temporary{}; const auto success = read(native_model, offset, 1, temporary);
    value = static_cast<UCHAR>(temporary); return success;
}
bool read_mmio16(const DeviceResources&, ULONG offset, USHORT& value) noexcept
{
    std::uint32_t temporary{}; const auto success = read(native_model, offset, 2, temporary);
    value = static_cast<USHORT>(temporary); return success;
}
bool read_mmio32(const DeviceResources&, ULONG offset, ULONG& value) noexcept
{
    std::uint32_t temporary{}; const auto success = read(native_model, offset, 4, temporary);
    value = temporary; return success;
}
bool write_mmio8(const DeviceResources&, ULONG offset, UCHAR value) noexcept { return write(native_model, offset, 1, value); }
bool write_mmio16(const DeviceResources&, ULONG offset, USHORT value) noexcept { return write(native_model, offset, 2, value); }
bool write_mmio32(const DeviceResources&, ULONG offset, ULONG value) noexcept { return write(native_model, offset, 4, value); }
}
#define RTL8125_MOCK(name, bytes, implementation) \
    decltype(&name) import_##name __asm__("__imp__" #name "@" #bytes) = implementation
extern "C" {
RTL8125_MOCK(KeGetCurrentIrql, 0, mock_irql);
RTL8125_MOCK(KeQueryInterruptTime, 0, mock_time);
RTL8125_MOCK(NdisMSleep, 4, mock_sleep);
RTL8125_MOCK(KeStallExecutionProcessor, 4, mock_stall);
RTL8125_MOCK(NdisReadPciSlotInformation, 20, mock_pci_read);
RTL8125_MOCK(NdisWritePciSlotInformation, 20, mock_pci_write);
RTL8125_MOCK(ZwCreateFile, 44, mock_create);
RTL8125_MOCK(ZwQueryInformationFile, 20, mock_query);
RTL8125_MOCK(ZwReadFile, 36, mock_file_read);
RTL8125_MOCK(ZwClose, 4, mock_close);
RTL8125_MOCK(ExAllocatePoolWithTag, 12, mock_allocate);
RTL8125_MOCK(ExFreePoolWithTag, 8, mock_free);
}
#endif
