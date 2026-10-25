// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tools.vfio_check
Purpose: Inspect a dedicated RTL8125 passthrough candidate without changing bindings.
Created: 2026-10-24
Architecture: Linux read-only test setup; never detaches, resets or starts a VM.
*/
#include "../core/chip.hpp"

#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

bool valid_address(const std::string& address)
{
    if (address.size() != 12 || address[4] != ':' || address[7] != ':' || address[10] != '.') {
        return false;
    }

    for (std::size_t index = 0; index < address.size(); ++index) {
        if (index == 4 || index == 7 || index == 10) {
            continue;
        }

        const char character = address[index];

        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F'))) {
            return false;
        }
    }

    unsigned slot{};
    const auto parsed = std::from_chars(address.data() + 8, address.data() + 10, slot, 16);
    return parsed.ec == std::errc{} && slot <= 31 && address[11] <= '7';
}

bool read_hex(const fs::path& path, unsigned& value)
{
    std::ifstream input(path, std::ios::binary);
    std::array<char, 32> buffer{};

    if (!input) {
        return false;
    }

    input.getline(buffer.data(), buffer.size());

    if (!input || buffer[0] != '0' || buffer[1] != 'x') {
        return false;
    }

    const std::string text(buffer.data());
    const auto parsed = std::from_chars(text.data() + 2, text.data() + text.size(), value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::string bound_driver(const fs::path& device)
{
    std::error_code error;
    const auto target = fs::read_symlink(device / "driver", error);
    return error ? "unbound" : target.filename().string();
}

bool eligible_group_status(const vfio_group_status& status) noexcept
{
    return status.argsz >= sizeof(status) &&
           (status.flags & VFIO_GROUP_FLAGS_VIABLE) != 0 &&
           (status.flags & VFIO_GROUP_FLAGS_CONTAINER_SET) == 0;
}

// VFIO_GROUP_GET_STATUS only retrieves group metadata. No container, IOMMU,
// device FD, interrupt, reset or DMA-map operation is requested. Permission bits
// alone cannot establish viability or whether another VM owns the group.
bool query_group_eligibility(const fs::path& group_node)
{
    const auto descriptor = ::open(group_node.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (descriptor < 0) {
        std::fputs("Rejected: cannot open the VFIO group for a read-only status query.\n", stderr);
        return false;
    }

    vfio_group_status status{};
    status.argsz = sizeof(status);
    struct stat node{};
    const bool queried = ::fstat(descriptor, &node) == 0 && S_ISCHR(node.st_mode) &&
                         ::ioctl(descriptor, VFIO_GROUP_GET_STATUS, &status) == 0;
    const auto close_status = ::close(descriptor);

    if (!queried || close_status != 0) {
        std::fputs("Rejected: VFIO group status could not be read from a character device.\n", stderr);
        return false;
    }

    if (!eligible_group_status(status)) {
        std::fputs("Rejected: VFIO group is not viable or already belongs to a container.\n", stderr);
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || !valid_address(argv[1])) {
        std::fputs("Usage: rtl8125_vfio_check DOMAIN:BUS:SLOT.FUNCTION\n"
                   "Read-only check; requires an already dedicated vfio-pci device.\n", stderr);
        return 2;
    }

    const std::string address(argv[1]);
    const fs::path device = fs::path("/sys/bus/pci/devices") / address;
    unsigned vendor{};
    unsigned product{};

    if (!read_hex(device / "vendor", vendor) || !read_hex(device / "device", product) ||
        vendor != rtl8125::realtek_vendor_id || product != rtl8125::rtl8125_device_id) {
        std::fputs("Rejected: this address is not an accessible RTL8125 (10ec:8125).\n", stderr);
        return 1;
    }

    const auto driver = bound_driver(device);
    std::printf("RTL8125 %s, current driver %s\n", address.c_str(), driver.c_str());
    bool ready = driver == "vfio-pci";
    std::error_code error;

    if (fs::exists(device / "net", error)) {
        fs::directory_iterator interfaces(device / "net", error);

        if (error) {
            ready = false;
        } else {
            for (; interfaces != fs::directory_iterator{} && !error; interfaces.increment(error)) {
                std::printf("Host interface present: %s\n",
                            interfaces->path().filename().string().c_str());
                ready = false;
            }

            ready = ready && !error;
        }
    } else if (error) {
        ready = false;
    }

    const auto group = fs::canonical(device / "iommu_group", error);

    if (error) {
        std::fputs("Rejected: no accessible IOMMU group.\n", stderr);
        return 1;
    }

    const auto group_name = group.filename().string();
    std::printf("IOMMU group %s\n", group_name.c_str());
    fs::directory_iterator members(group / "devices", error);

    if (error) {
        std::fputs("Rejected: cannot enumerate the complete IOMMU group.\n", stderr);
        return 1;
    }

    bool target_found = false;

    for (; members != fs::directory_iterator{} && !error; members.increment(error)) {
        const auto name = members->path().filename().string();

        if (name == address) {
            target_found = true;
            continue;
        }

        unsigned device_class{};

        // PCI bridges may appear in the group; another endpoint makes this
        // simple single-device configuration ineligible. No group is rebound.
        if (!read_hex(members->path() / "class", device_class) ||
            (device_class >> 8) != 0x0604) {
            std::printf("Rejected: other endpoint %s shares the group.\n", name.c_str());
            ready = false;
        }
    }

    ready = ready && target_found && !error;
    const auto group_node = fs::path("/dev/vfio") / group_name;

    if (::access("/dev/kvm", R_OK | W_OK) != 0 ||
        ::access(group_node.c_str(), R_OK | W_OK) != 0) {
        std::fputs("Rejected: KVM/VFIO group is not accessible to this process.\n", stderr);
        ready = false;
    }

    if (ready && !query_group_eligibility(group_node)) {
        ready = false;
    }

    if (!ready) {
        std::fputs("Candidate is not eligible for launch. Supply a dedicated, already VFIO-bound\n"
                   "RTL8125 with no host network interface and an isolated usable group.\n"
                   "This tool changed no device, binding or network state.\n", stderr);
        return 1;
    }

    std::printf("Candidate meets setup checks. QEMU device argument: -device vfio-pci,host=%s\n",
                address.c_str());
    std::puts("VFIO group status is viable and unassigned at this check; state may change before launch.");
    std::puts("Actual ReactOS resource/XID probing and serial capture still required.");
    return 0;
}
