// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.hardware.phy
Purpose: Faithful RTL8125A/B PHY tuning and EPHY table sub-sequences.
Created: 2026-10-24
Architecture: device.cpp owns full sequencing and autonegotiation.
Derived from Linux v6.12 r8169_phy_config.c and r8169_main.c.
*/
#pragma once
#include "firmware.hpp"
#include "../core/chip.hpp"

namespace rtl8125::hardware {

// PASSIVE_LEVEL natively, exclusive scope. Applies the exact A/B EPHY entries
// only; the caller must first program Linux ASPM entry latency through PCI/CSI.
// Refuses unsupported identity. Failure requires full recovery before retry.
bool configure_ephy(RegisterSession&, MacVersion) noexcept;

// PASSIVE_LEVEL. Ports rtl8125b_hw_phy_config in upstream order, including
// firmware, soft-reset polling, PHY parameters, legacy force mode, ALDPS and
// PHY EEE tuning. Only RTL8125B; full MAC/OOB/ASPM sequencing is a separate gate.
// Does not claim link or advertise speed. Caller must have identified B and
// acquired the exact B firmware. A failure leaves the PHY unusable until reset.
// The A sequence includes the exact 15 coefficient writes and 25 zero writes,
// PHY parameters, firmware placement, GPHY 10M and EEE tuning from Linux.
// Caller must identify A and acquire the matching A firmware before this scope.
bool configure_rtl8125a_phy(RegisterSession&, const FirmwareView&) noexcept;

bool configure_rtl8125b_phy(RegisterSession&, const FirmwareView&) noexcept;

} // namespace rtl8125::hardware
