// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.tests.resources_vfio_check
Purpose: Verify actual preflight parsing and fail-closed VFIO group-status policy.
Created: 2026-10-24
Architecture: Linux host tests only; no real VFIO group, NIC or VM is accessed.
*/
#define main rtl8125_vfio_check_entry_for_test
#include "../src/tools/vfio_check.cpp"
#undef main

#include <cstring>

namespace {

unsigned checks{};
unsigned failures{};

void expect(bool condition, const char* message) noexcept
{
    ++checks;

    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void check_status() noexcept
{
    vfio_group_status status{};
    status.argsz = sizeof(status);
    expect(!eligible_group_status(status), "nonviable unassigned group rejected");
    status.flags = VFIO_GROUP_FLAGS_CONTAINER_SET;
    expect(!eligible_group_status(status), "nonviable assigned group rejected");
    status.flags = VFIO_GROUP_FLAGS_VIABLE | VFIO_GROUP_FLAGS_CONTAINER_SET;
    expect(!eligible_group_status(status), "viable group owned by another container rejected");
    status.flags = VFIO_GROUP_FLAGS_VIABLE;
    expect(eligible_group_status(status), "viable unassigned group passes status policy only");
    status.argsz = sizeof(status) - 1;
    expect(!eligible_group_status(status), "short kernel status response rejected");
    status.argsz = 0;
    expect(!eligible_group_status(status), "empty kernel status response rejected");
}

void check_addresses() noexcept
{
    const char* valid[]{"0000:00:00.0", "ffff:ff:1f.7", "ABCD:EF:1A.7", "0000:0d:00.0"};

    for (const auto* address : valid) {
        expect(valid_address(address), "valid complete PCI address recognized");
    }

    const char* invalid[]{"", "0d:00.0", "0000:0d:20.0", "0000:0d:00.8", "0000:0d:00.a",
                          "0000:0d:00.A", "0000:0g:00.0", "0000/0d:00.0", "0000:0d:00:0",
                          "0000:0d:00.0/", "../0:0d:00.0", " 0000:0d:00.0", "0000:0d:00.0\n"};

    for (const auto* address : invalid) {
        expect(!valid_address(address), "invalid PCI/path input rejected");
    }
}

void check_query_failures() noexcept
{
    expect(!query_group_eligibility("/dev/null"), "non-VFIO character device ioctl failure rejected");

    char temporary[] = "/tmp/rtl8125-vfio-check-XXXXXX";
    const auto descriptor = ::mkstemp(temporary);
    expect(descriptor >= 0, "owned regular-file test fixture created");

    if (descriptor < 0) {
        return;
    }

    ::close(descriptor);
    expect(!query_group_eligibility(temporary), "regular file cannot impersonate a VFIO group");
    ::unlink(temporary);
    expect(!query_group_eligibility(temporary), "missing group node rejected");

    if (::symlink("/dev/null", temporary) == 0) {
        expect(!query_group_eligibility(temporary), "symlink group node rejected without following");
        ::unlink(temporary);
    } else {
        expect(false, "owned symlink test fixture created");
    }
}

} // namespace

int main()
{
    check_status();
    check_addresses();
    check_query_failures();
    std::printf("RTL8125 VFIO preflight contracts: %u checks, %u failures (no physical group access)\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
