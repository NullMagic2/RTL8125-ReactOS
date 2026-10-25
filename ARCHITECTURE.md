<!--
Module: rtl8125.documentation.architecture
Purpose: Define Linux-derived algorithms and verified native NDIS ownership boundaries.
Created: 2026-10-24
Adapted from: docs/imported/rx7900/ARCHITECTURE.md; conventions only, no GPU runtime code.
-->
# Architecture

## Overview

RTL8125-ReactOS ports the RTL8125A/B subset of Linux r8169 into C++17 for an initial i386 NDIS5.1 driver. Linux v6.12 is pinned to adc218676eef25575469234709c2d87185ca223a with unchanged references and SHA-256 provenance. Linux identifies host PCI10ec:8125 rev05, subsystem1462:7e27, BDF0000:0d:00.0 as RTL8125B XID641 with a64KiB BAR2. Native source owns bounded assigned resources, typed MMIO, conservative discovery and driver-object wrapper unload. M01/M02 source is complete. Required statistics negotiate64/32-bit replies; counters never invent traffic. The default probe build performs only a read-only XID probe and returns NOT_SUPPORTED. The separate operational build implements hardware startup, DMA, interrupts and recovery, and requires the three compatible ReactOS NDIS patches. The probe never activates hardware or advertises a working network adapter.

M03 source is complete: bounded exclusive PCI/CSI/OCP/EPHY/MDIO services, exact A/B MAC/PHY/FIFO/OOB/reset tuning, fixed-name firmware loading/interpreter, real station identity, autonegotiation and measured link state. Prepare and activate are distinct; activation preserves packet filters and MTU1500 receive limits. ASPM/CLKREQ and EEE/pause advertising stay disabled as explicit unqualified-platform policy. Short native delays use bounded processor stalls; longer waits sleep at PASSIVE_LEVEL. M05/M06 source is complete: Adapter owns actual hardware state, DMA buffers and Datapath; production Send, OIDs, statistics, bounded packet DPC dispatch, PnP/shutdown admission and Halt ownership call those same modules. Common buffers hold fixed aligned rings and copied packets; OWN is published last with native barriers. Pending sends complete once; RX uses preallocated storage and synchronous NDIS_STATUS_RESOURCES. Failure to prove retirement retains all live state and mappings. M07 and M08 source is complete: assigned INTx/private DPC ownership, joined recovery/link worker, real reset/power restoration and full CPU/device retirement. Three ordered ReactOS NDIS patches correct Halt context, callback rundown and device-power dispatch; they were compiled/linked only in an owned source mirror. The shared external SDK remains unchanged. The three-patch NDIS binary now boots on an owned copy of the selected guest; its approved image passes the installer’s cross-boot check. No OS patch is assumed for common-buffer DMA; INTx/MSI delivery and the inspected legacy RX-wrapper limitations still need validation.

Source implementation is 100% for M00–M09; M10 is validation-only and physical qualification remains 0%. Testing is excluded from code scoring. The operational i386 candidate includes real startup, exact INF, install/remove/recovery instructions, full source and the three required NDIS patches. GCC13 operational build/PE/package checks passed; new 85 concurrent recovery checks, 42 actual compiled patched-NDIS checks and 1,065 package sanitizer checks passed. These are scoped model/ABI/source results, not operational guest or hardware validation. Historical M01 and packet-linked guest regressions remain separately captured; no final operational guest installation, MSVC execution, real RTL8125 traffic or hardware reset/power qualification is claimed. A separate standalone package uses Qemu-Lightning's generic package engine to bundle approved NDIS, A/B firmware, licenses and driver payloads in one executable with guest-local reboot resume; its packaging entry is cmake/standalone.cmake. The earlier operational candidate includes a native C++17 Win32 installer: exact RTL8125B identification for the verified MSI hardware ID, one-device SetupAPI binding, firmware validation, approved NDIS hash and earlier-boot checks, progress and scoped recovery. Its GUI was preview-checked at normal and 800x600 sizes; 43 policy, 11 actual-backend preview/denial and 60 independent SHA256 boundary checks passed alongside 1,065 package checks. Preview and Wine cannot enter installation operations. Final operational guest installation remains pending; the older read-only probe remains a separate artifact. Qemu-Lightning already supports VirtIO/shared-host networking. The preserved guest now automatically installs its bundled NetKVM driver, obtains 10.0.3.15 through DHCP and passes a TCP transfer explicitly bound to the VirtIO adapter. This separate adapter does not exercise the RTL8125 driver, which has no VirtIO backend. The sole Linux NIC, external SDK and immutable VM base remain unchanged. The final shared-network preset omits the old RTL8139 device to avoid its stale default route; the previous launch remains available for rollback.

```text
rtl8125-reactos/
|-- CMakeLists.txt / roadmap.json / driver-roadmap.html
|-- STYLE_GUIDE.md / ARCHITECTURE.md / THIRD_PARTY.txt / LICENSE
|-- cmake/standalone.cmake            Package verified prerequisites with the generic setup engine
|-- src/
|   |-- core/chip.cpp
|   |-- core/chip.hpp
|   |-- core/descriptor.cpp
|   |-- core/descriptor.hpp
|   |-- core/descriptor_bits.hpp
|   |-- core/filter.cpp
|   |-- core/filter.hpp
|   |-- core/frame_limits.hpp
|   |-- core/oid.cpp
|   |-- core/oid.hpp
|   |-- core/packet.cpp
|   |-- core/packet.hpp
|   |-- core/probe_log.cpp
|   |-- core/probe_log.hpp
|   |-- core/receive.cpp
|   |-- core/receive.hpp
|   |-- core/resources.cpp
|   |-- core/resources.hpp
|   |-- hardware/device.cpp
|   |-- hardware/device.hpp
|   |-- hardware/firmware.cpp
|   |-- hardware/firmware.hpp
|   |-- hardware/phy.cpp
|   |-- hardware/phy.hpp
|   |-- hardware/registers.cpp
|   |-- hardware/registers.hpp
|   |-- installer/backend.cpp
|   |-- installer/backend.hpp
|   |-- installer/main.cpp
|   |-- installer/model.cpp
|   |-- installer/model.hpp
|   |-- installer/package_contract.hpp
|   |-- reactos/adapter.hpp
|   |-- reactos/datapath.cpp
|   |-- reactos/datapath.hpp
|   |-- reactos/datapath_adapter.cpp
|   |-- reactos/dma.cpp
|   |-- reactos/dma.hpp
|   |-- reactos/hardware_init.cpp
|   |-- reactos/hardware_init.hpp
|   |-- reactos/hardware_io.cpp
|   |-- reactos/interrupt.cpp
|   |-- reactos/interrupt.hpp
|   |-- reactos/miniport.cpp
|   |-- reactos/recovery.cpp
|   |-- reactos/recovery.hpp
|   |-- reactos/recovery_cpu.cpp
|   |-- reactos/recovery_cpu.hpp
|   |-- reactos/resources.cpp
|   |-- reactos/resources.hpp
|   |-- tools/probe_collect.cpp
|   |-- tools/vfio_check.cpp
|-- tests/
|   |-- core_tests.cpp
|   |-- datapath_import_stubs.cpp
|   |-- datapath_native_tests.cpp
|   |-- dma_native_tests.cpp
|   |-- firmware_file_check.cpp
|   |-- hardware_device_tests.cpp
|   |-- hardware_tests.cpp
|   |-- installer_preview_tests.cpp
|   |-- installer_tests.cpp
|   |-- interrupt_import_stubs.cpp
|   |-- interrupt_native_tests.cpp
|   |-- miniport_contract_tests.cpp
|   |-- miniport_guest_kernel.cpp
|   |-- miniport_guest_runner.cpp
|   |-- oid_tests.cpp
|   |-- package_check.cpp
|   |-- package_tests.cpp
|   |-- packet_tests.cpp
|   |-- probe_log_tests.cpp
|   |-- recovery_import_stubs.cpp
|   |-- recovery_native_tests.cpp
|   |-- recovery_ndis_tests.cpp
|   |-- resources_native_tests.cpp
|   |-- resources_tests.cpp
|   |-- resources_vfio_check_tests.cpp
|-- cmake/
|   |-- datapath.cmake
|   |-- driver_sources.cmake
|   |-- hardware.cmake
|   |-- installer.cmake
|   |-- interrupt.cmake
|   |-- native.cmake
|   |-- package.cmake
|   |-- recovery.cmake
|-- packaging/
|   |-- BUILD.txt
|   |-- FIRMWARE.txt
|   |-- INSTALL.txt
|   |-- installer_package.cmake
|   |-- REACTOS_COMPATIBILITY.txt
|   |-- SETUP.txt
|   |-- notices/REALTEK.txt
|   |-- notices/compiler/             Unchanged compiler runtime notices and origin
|   |-- reactos/CMakeLists.txt
|   |-- rtl8125.inf
|   |-- stage.cmake
|-- patches/
|   |-- reactos/0001-ndis-halt-context.patch
|   |-- reactos/0002-ndis-callback-rundown.patch
|   |-- reactos/0003-ndis-device-power.patch
|-- docs/imported/rx7900/             Unchanged requested documents
|-- third_party/qemu-lightning-guest-packages/  Unchanged shared planner and NT5 setup sources
|-- packaging/standalone/            Approved catalog, firmware notice and standalone build instructions
|-- third_party/linux/               Unchanged pinned reference sources
|-- evidence/                        Scoped build/model/guest/provenance evidence
|-- validation/shared-network/network_probe.cpp  Source-bound NetKVM guest diagnostic
|-- validation/shared-network/README.txt         Shared-network setup and evidence
|-- validation/physical/             Older read-only probe validation setup
`-- Binaries/                        Published probe and staged operational candidate
```
