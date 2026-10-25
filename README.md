# RTL8125-ReactOS

Experimental Realtek RTL8125A/B 2.5 GbE drivers and a native installer for ReactOS i386 (NDIS 5.1).

**These drivers are tailor-made for ReactOS and may not be fully compatible with Windows. The drivers, interfaces, and installation requirements are subject to change.**

## Status

The RTL8125A/B implementation is complete in source. Physical-card validation is still pending: successful RTL8125 Ethernet traffic, throughput, interrupt delivery, reset, and power recovery have not been demonstrated. Treat releases as experimental development builds.

The confirmed target PCI ID is `10ec:8125`. The verified subsystem `7e27:1462`, revision `05` is identified as **RTL8125B** in the installer and Device Manager. Other variants remain RTL8125 until the driver identifies their MAC family.

The ReactOS VM’s separate **VirtIO/NetKVM** connection passed DHCP, default-route selection, and a TCP/HTTP transfer bound to that virtual adapter. The patched NDIS image also passed an actual boot/reboot compatibility check. **VirtIO results do not test this RTL8125 driver, which has no VirtIO backend.**

## Downloads and installation

Download the complete i386 ZIP from [Releases](https://github.com/NullMagic2/RTL8125-ReactOS/releases). Extract it before running `RTL8125-Setup.exe` inside a recoverable ReactOS test system as an administrator. The standalone executable also requires its matching `rtl8125.sys`, `rtl8125.inf`, and the package’s supporting files beside it.

Requirements:

- ReactOS i386 and administrator access. Native AMD64 is not implemented.
- An actual RTL8125 PCI device available to ReactOS. A virtual VirtIO adapter uses NetKVM instead.
- A matching patched ReactOS NDIS build. Apply the three ordered patches in [patches/reactos](patches/reactos) and read [REACTOS_COMPATIBILITY.txt](packaging/REACTOS_COMPATIBILITY.txt). The installer requires its reviewed NDIS binary hash and verification across a real reboot.
- Separately obtained, uncompressed Realtek firmware: RTL8125B uses `rtl8125b-2.fw`; an unidentified RTL8125 requires both A/B firmware files for preflight. Firmware is not included or downloaded. See [FIRMWARE.txt](packaging/FIRMWARE.txt).

Two installer packages are provided:

| Package | Journal handling |
| --- | --- |
| `RTL8125B-ReactOS-Setup-i386.zip` | Requires protected machine registry journals; rejects ReactOS builds whose registry security is stubbed. |
| `RTL8125B-ReactOS-Controlled-VM-i386.zip` | Explicit trusted test guest variant. Accepts the audited ReactOS ACL stub; journal permissions cannot be enforced. The window title says **Controlled VM**. |

The controlled variant retains approved NDIS hash, payload integrity, actual reboot, and per-device checks. Use it only in an isolated trusted test guest. It does not make the driver hardware-qualified. The installer never replaces core OS files.

Read [SETUP.txt](packaging/SETUP.txt) and [INSTALL.txt](packaging/INSTALL.txt) for readiness checks, recovery, and current ReactOS removal limitations. Keep a recoverable VM disk or system backup. A source-only completion percentage does not establish working hardware.

## Build

Portable checks need a C++17 compiler and CMake:

```sh
cmake -S . -B build -DRTL8125_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Building the native driver also requires matching ReactOS i386 sources, generated SDK headers, and import libraries. See [BUILD.txt](packaging/BUILD.txt) for GCC/MinGW and ReactOS/MSVC integration instructions. MSVC execution remains unverified.

```sh
cmake -S . -B build-driver \
  -DRTL8125_TESTS=OFF -DRTL8125_NATIVE_DRIVER=ON \
  -DRTL8125_OPERATIONAL=ON -DRTL8125_REACTOS_WRAPPER_PATCHED=ON \
  -DRTL8125_REACTOS_SOURCE=/path/to/reactos/source \
  -DRTL8125_REACTOS_BUILD=/path/to/reactos/build
cmake --build build-driver --target rtl8125_candidate_package -j2
```

For the GUI, configure `RTL8125_INSTALLER=ON` and `RTL8125_INSTALLER_DRIVER` with the actual operational SYS path. `RTL8125_INSTALLER_TRUSTED_GUEST=ON` explicitly selects the controlled VM variant; the default is OFF. `RTL8125_INSTALLER_NDIS_HASH` pins the reviewed patched NDIS binary. Do not replace it with an unreviewed hash to bypass preflight.

## Source and validation

- [Architecture](ARCHITECTURE.md) and [implementation ledger](roadmap.json).
- [Linux source provenance and attribution](THIRD_PARTY.txt).
- [Guest shared-network results](evidence/shared-network.json) and [actual guest output](evidence/shared-network-guest.txt).
- [Installer and NDIS guest preparation](evidence/vm-installation.json).

Release ZIPs include matching source archives, license notices, required ReactOS patches, and SHA-256 manifests. VM disks, local build caches, STYLE_GUIDE.md, and agent instruction files are excluded from publication.

## License

The driver and combined project are distributed under **GNU GPL version 2 only (`GPL-2.0-only`)**. See [LICENSE](LICENSE).

This port adapts Linux v6.12 `r8169` code. Upstream notices and per-file licenses remain intact; some upstream reference files permit GPL v2 or later. Compiler runtime notices and separately obtained Realtek firmware retain their respective terms. This is a community ReactOS port, not an official Realtek driver package.
