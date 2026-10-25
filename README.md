# RTL8125-ReactOS

Experimental Realtek RTL8125A/B 2.5 GbE drivers and a native installer for ReactOS i386 (NDIS 5.1).

**These drivers are tailor-made for ReactOS and may not be fully compatible with Windows. The drivers, interfaces, and installation requirements are subject to change.**


## Downloads and installation

Download **[RTL8125B-ReactOS-Standalone-i386.zip](https://github.com/NullMagic2/RTL8125-ReactOS/releases/download/v0.1.0-alpha.2/RTL8125B-ReactOS-Standalone-i386.zip)**.

1. Extract the ZIP.
2. Run **RTL8125B-Setup.exe** as administrator inside **ReactOS i386**.
3. Restart when prompted. Setup resumes automatically at administrator sign-in.

This single package includes the driver, firmware, approved NDIS compatibility update, license notices, and matching source. No Qemu-Lightning manager preparation, separate firmware download, or additional installer is needed.

ReactOS needs access to an actual RTL8125 card to use this driver. Setup stages the driver if the card is absent. Native AMD64 is not implemented, and unknown OS compatibility baselines are rejected.

See [SETUP.txt](packaging/SETUP.txt) for recovery details.

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

To build the single installer package, use the `rtl8125_standalone` target described in [packaging/standalone/BUILD.txt](packaging/standalone/BUILD.txt).


## License

The driver and combined project are distributed under **GNU GPL version 2 only (`GPL-2.0-only`)**. See [LICENSE](LICENSE).

This port adapts Linux v6.12 `r8169` code. Upstream notices and per-file licenses remain intact; some upstream reference files permit GPL v2 or later. Compiler runtime notices and Realtek firmware retain their respective terms. This is a community ReactOS port, not an official Realtek driver package.
