// SPDX-License-Identifier: GPL-2.0-only
/*
Module: rtl8125.core.resources
Purpose: Decode PCI/NT resource values with exact buffer and arithmetic bounds.
Created: 2026-10-24
Architecture: Implements resources.hpp; no NT headers, allocation or device I/O.
Linux first-memory-BAR policy derives from rtl_init_one; see THIRD_PARTY.txt.
*/
#include "resources.hpp"
#include "chip.hpp"

namespace rtl8125 {
namespace {

std::uint16_t read16(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint16_t>(bytes[0] |
                                    (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::uint32_t read32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(read16(bytes)) |
           (static_cast<std::uint32_t>(read16(bytes + 2)) << 16);
}

std::uint64_t read64(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint64_t>(read32(bytes)) |
           (static_cast<std::uint64_t>(read32(bytes + 4)) << 32);
}

} // namespace

bool decode_register_bar(const std::uint8_t* config, std::size_t size,
                         PciRegisterBar& bar) noexcept
{
    bar = {};

    if (config == nullptr || size < pci_config_snapshot_size ||
        read16(config) != realtek_vendor_id || read16(config + 2) != rtl8125_device_id ||
        (config[14] & 0x7fU) != 0 || config[11] != 2 || config[10] != 0 ||
        (read16(config + 4) & 2U) == 0) {
        return false;
    }

    for (std::uint8_t index = 0; index < 6; ++index) {
        const auto low = read32(config + 16 + 4 * index);

        if (low == 0 || low == UINT32_MAX || (low & 1U) != 0) {
            continue;
        }

        const auto type = low & 6U;

        if (type != 0 && type != 4) {
            return false;
        }

        std::uint64_t address = low & ~std::uint32_t{15};

        if (type == 4) {
            if (index == 5) {
                return false;
            }

            address |= static_cast<std::uint64_t>(read32(config + 20 + 4 * index)) << 32;
        }

        // Prefetchable/device-write-combining mappings are not used for control
        // registers. Unsupported first memory BARs must not fall through to ROM.
        if (address == 0 || (low & 8U) != 0) {
            return false;
        }

        bar = {address, index, config[8], type == 4};
        return true;
    }

    return false;
}

bool decode_assigned_resources(const std::uint8_t* bytes, std::size_t size,
                               std::size_t descriptor_stride,
                               const PciRegisterBar& bar,
                               AssignedResources& resources) noexcept
{
    resources = {};

    if (bytes == nullptr || size < 8 || size > maximum_resource_list_size ||
        (descriptor_stride != 16 && descriptor_stride != 20) || bar.address == 0 ||
        read16(bytes) != 1 || read16(bytes + 2) != 1) {
        return false;
    }

    const auto count = read32(bytes + 4);

    if (count == 0 || count > (size - 8) / descriptor_stride ||
        8 + count * descriptor_stride != size) {
        return false;
    }

    AssignedResources candidate{};
    bool memory_found = false;
    bool interrupt_found = false;

    for (std::uint32_t index = 0; index < count; ++index) {
        const auto* descriptor = bytes + 8 + index * descriptor_stride;
        const auto type = descriptor[0];
        const auto flags = read16(descriptor + 2);

        if (type == 5 || type == 7) {
            // DeviceSpecific data has a variable tail; MemoryLarge needs another
            // length encoding. Neither is required for these RTL8125 BARs.
            return false;
        }

        if (type == 3) {
            const auto address = read64(descriptor + 4);
            const auto length = read32(descriptor + 12);

            if (length == 0 || address > UINT64_MAX - (length - 1U)) {
                return false;
            }

            if (address == bar.address) {
                // READ_ONLY, WRITE_ONLY, PREFETCHABLE, COMBINEDWRITE,
                // CACHEABLE and unsupported large-memory flags are forbidden.
                if (memory_found || length < minimum_register_aperture ||
                    (flags & ~0x0080U) != 0) {
                    return false;
                }

                memory_found = true;
                candidate.address = address;
                candidate.length = length;
            } else if (bar.address > address && bar.address - address < length) {
                // Do not map a subrange of an unrelated encompassing resource.
                return false;
            }
        } else if (type == 2) {
            const auto level = read32(descriptor + 4);
            const auto vector = read32(descriptor + 8);
            const auto affinity = descriptor_stride == 20 ? read64(descriptor + 12) :
                                  static_cast<std::uint64_t>(read32(descriptor + 12));

            if (interrupt_found || (flags & ~1U) != 0 ||
                descriptor[1] > 3 || vector == 0 || level == 0 || affinity == 0) {
                return false;
            }

            interrupt_found = true;
            candidate.interrupt_level = level;
            candidate.interrupt_vector = vector;
            candidate.interrupt_affinity = affinity;
            candidate.interrupt_flags = flags;
            candidate.interrupt_shared = descriptor[1] == 3;
        }
    }

    if (!memory_found || !interrupt_found) {
        return false;
    }

    const auto candidate_end = candidate.address + candidate.length - 1U;

    // Other assigned memory ranges may precede the selected BAR in the list.
    // Once its exact aperture is known, reject any overlap in either direction;
    // checking only whether another range contains the BAR start misses aliases
    // that begin inside this register window. The first pass bounded all ranges.
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto* descriptor = bytes + 8 + index * descriptor_stride;

        if (descriptor[0] != 3) {
            continue;
        }

        const auto address = read64(descriptor + 4);

        if (address == candidate.address) {
            continue;
        }

        const auto end = address + read32(descriptor + 12) - 1U;

        if (address <= candidate_end && candidate.address <= end) {
            return false;
        }
    }

    resources = candidate;
    return true;
}

bool valid_mmio_access(std::uint32_t length, std::uint32_t offset,
                       std::uint32_t width) noexcept
{
    return (width == 1 || width == 2 || width == 4) && (offset % width) == 0 &&
           offset <= length && width <= length - offset;
}

} // namespace rtl8125
