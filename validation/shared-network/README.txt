Module: rtl8125.validation.shared_network
Purpose: Reproduce the selected guest's shared VirtIO setup and scoped verification.
Created: 2026-10-24
Architecture: Bundled ReactOS NetKVM talks to Qemu-Lightning's existing VirtIO device.

The RTL8125A/B driver in this project has no VirtIO backend. This setup provides
shared guest networking and does not validate that driver's physical-card path.
The physical RTL8125B stays bound to Linux r8169; no VFIO handoff is performed.

Actual local setup
  Binaries/.build/shared-network/network.cfg is the owned QEMU preset.
  It selects virtio-net-pci with disable-modern=on, disable-legacy=off, vectors=0,
  PCI slot 0x7, MAC 52:54:00:81:25:01 and user NAT 10.0.3.0/24.
  ReactOS automatically installs its bundled netkvm.sys for PCI 1af4:1000.
  The emulated RTL8139 is omitted from the final configuration: ReactOS retained
  its previous default route after virtual link disconnection. Previous launch
  settings are preserved in pre-shared-network-launch.json for rollback.
  The final guest uses an owned qcow2 overlay of the prepared disk.
  Qemu-Lightning already supports VirtIO and shared NAT; no source change needed.

Build diagnostic (from project root, i686 MinGW C++ compiler)
  i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions
    -fno-rtti -static -D_WIN32_WINNT=0x0502
    validation/shared-network/network_probe.cpp -lsetupapi -lcfgmgr32
    -liphlpapi -lws2_32 -o network-probe.exe
  Join the wrapped arguments onto one command line. No kernel SDK is required.

Run network-probe.exe in the guest without arguments for read-only inspection.
It reports NetKVM presence, PnP service/start status, adapters, the IPv4 address
of the exact configured VirtIO MAC and whether VirtIO is the default route.
For transfer verification, pass a 64-character expected token as its argument.
Serve that token followed by one newline as /token.txt from a temporary HTTP
server bound only to host 127.0.0.1:18425. QEMU exposes the host at 10.0.3.2.
The test requires a started NetKVM device, binds TCP explicitly to the VirtIO
IPv4 address, and verifies HTTP 200 plus the exact body. The RTL8139 cannot
satisfy this transfer check. The token is test data, not a credential.
Stop the temporary server after verification. No external server is required.

Actual results and limitations
  evidence/shared-network.json records the setup and current guest observations.
  evidence/shared-network-guest.txt and shared-network-http.txt preserve both ends.
  A successful transfer verifies only this virtual network path, not RTL8125
  hardware, installer binding, throughput, power recovery or general qualification.
