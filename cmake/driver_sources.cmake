# Module: rtl8125.build.driver_sources
# Purpose: Share the exact production source inventory between native build routes.
# Created: 2026-10-24
# Architecture: Relative to the driver source root, never a ReactOS source root.
set(native_packet_sources src/core/packet.cpp src/core/filter.cpp src/core/receive.cpp
    src/reactos/dma.cpp src/reactos/datapath.cpp src/reactos/datapath_adapter.cpp)
set(native_hardware_sources src/hardware/registers.cpp src/hardware/firmware.cpp
    src/hardware/phy.cpp src/hardware/device.cpp src/reactos/hardware_io.cpp src/reactos/hardware_init.cpp)
set(native_recovery_sources src/reactos/recovery.cpp src/reactos/recovery_cpu.cpp)
set(native_sources src/core/chip.cpp src/core/oid.cpp src/core/resources.cpp
    src/reactos/resources.cpp src/reactos/miniport.cpp src/reactos/interrupt.cpp
    ${native_packet_sources} ${native_hardware_sources} ${native_recovery_sources})
