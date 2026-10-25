// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.hardware.device
Purpose: Complete bounded RTL8125A/B MAC, PHY, autonegotiation and link services.
Created: 2026-10-24
Architecture: Exclusive portable sessions; explicit native activation boundary.
Derived from Linux v6.12 r8169_main.c and realtek_phy.c; see THIRD_PARTY.txt.
*/
#pragma once
#include "phy.hpp"
#include "../core/frame_limits.hpp"

namespace rtl8125::hardware {
struct DeviceState {
    MacVersion version{MacVersion::unsupported};
    std::uint8_t permanent_address[6]{};
    bool initialized{};
    bool running{};
    bool recovery_required{};
};
struct LinkState {
    bool up{};
    std::uint32_t speed_mbps{};
    bool full_duplex{};
    bool autoneg_complete{};
};
struct RingConfiguration {
    std::uint64_t transmit{};
    std::uint64_t receive{};
    std::uint32_t receive_buffer_size{static_cast<std::uint32_t>(dma_buffer_size)};
};
// PASSIVE_LEVEL, empty state and matching immutable validated firmware required.
// Owns no DMA memory. PCI basic configuration access must be available; extended
// config latency uses bounded CSI fallback. ASPM/CLKREQ and EEE stay disabled:
// NT platform ASPM arbitration and EEE have not been qualified. Preserves Linux
// tuning order, legacy descriptors and MTU1500 software offload policy. Leaves
// IRQs masked and RX/TX stopped. Any started failure requires separate recovery.
bool initialize_device(RegisterSession&, MacVersion, const FirmwareView&, DeviceState&) noexcept;
// Caller has already exposed/armed its genuine DMA owner, validated IRQ/recovery
// platform services and serialized all register owners. Writes high ring halves
// first, then starts engines; IRQ enable is owned by M07. No allocations/frees.
// Failure marks recovery required even if DMA may have become reachable.
bool activate_device(RegisterSession&, DeviceState&, const RingConfiguration&) noexcept;
// Bounded read-only PHY snapshot. Down/error clears speed and duplex; link is
// reported only after completed autonegotiation and a stable link reread. Actual
// downshift speed comes from Realtek PHY status, never configured advertisement.
bool read_link(RegisterSession&, const DeviceState&, LinkState&) noexcept;
// PASSIVE_LEVEL, validated A/B mapping including partially initialized hardware.
// Masks IRQs, rejects new RX, gates RXDV, follows the pinned variant-specific
// FIFO drain and settling delays, then resets and verifies both engines stopped.
// This is only the device portion of retirement: the caller must first fence all
// CPU users, then disable/read back PCI bus mastering and drain PCIe requests.
// Failure never permits descriptor reuse or common-buffer release.
bool quiesce_device(RegisterSession&, MacVersion) noexcept;
// Best effort containment in a NEW session after the failed one released. Masks
// IRQs, rejects RX and stops engines. Does not establish DMA retirement or grant
// permission to free buffers. Caller retains recovery_required after any failure.
bool contain_device(RegisterSession&, DeviceState&) noexcept;
} // namespace rtl8125::hardware
