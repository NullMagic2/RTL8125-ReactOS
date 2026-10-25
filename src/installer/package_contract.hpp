/*
Module: rtl8125.installer.package_contract
Purpose: Bound exact package metadata and actual PE checks before staging or installation.
Created: 2026-10-24
Architecture: Shared user-mode validation for the package tool and native installer.
*/
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rtl8125::package {

// A retained diagnostic identifies the compiled startup branch. This prevents
// staging the ordinary probe by accident; it does not prove successful startup.
inline bool operational_variant(const std::vector<std::uint8_t>& bytes)
{
    constexpr char marker[] = "RTL8125: operational adapter initialized\n";
    constexpr char probe[] = "RTL8125: probe XID %03lx, MAC family %lu; datapath incomplete\n";
    return std::search(bytes.begin(), bytes.end(), marker, marker + sizeof(marker)) != bytes.end() &&
        std::search(bytes.begin(), bytes.end(), probe, probe + sizeof(probe)) == bytes.end();
}

inline std::string normalized(std::string value)
{
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char byte) {
        return std::isspace(byte) != 0;
    }), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return value;
}

// Returns false for metadata outside the deliberately narrow exact-device INF.
inline bool validate_inf(const std::string& input)
{
    if (input.empty() || input.size() > 65536 || input.find('\0') != std::string::npos) {
        return false;
    }
    std::map<std::string, std::vector<std::string>> sections;
    std::string section;
    std::size_t position{};
    while (position < input.size()) {
        const auto end = input.find('\n', position);
        auto line = input.substr(position, end == std::string::npos ? end : end - position);
        position = end == std::string::npos ? input.size() : end + 1;
        line = normalized(line.substr(0, line.find(';')));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            if (section.empty() || sections.count(section) != 0) {
                return false;
            }
            sections.emplace(section, std::vector<std::string>{});
        } else if (section.empty()) {
            return false;
        } else {
            sections[section].push_back(line);
        }
    }
    const std::map<std::string, std::vector<std::string>> exact{
        {"version", {"signature=\"$windowsnt$\"", "class=net",
            "classguid={4d36e972-e325-11ce-bfc1-08002be10318}",
            "provider=%provider%", "driverver=10/24/2026,0.1.0.0"}},
        {"sourcedisksnames", {"1=\"rtl8125reactosexperimentali386driver\",,,"}},
        {"sourcedisksfiles", {"rtl8125.sys=1"}},
        {"destinationdirs", {"defaultdestdir=12"}},
        {"manufacturer", {"%hardwarevendor%=devices,ntx86"}},
        {"devices.ntx86", {"%deviceb%=rtl8125b_install.ndi,pci\\ven_10ec&dev_8125&subsys_7e271462&rev_05",
            "%device%=rtl8125_install.ndi,pci\\ven_10ec&dev_8125"}},
        {"rtl8125b_install.ndi.ntx86", {"characteristics=0x4", "bustype=5",
            "addreg=rtl8125_ndi", "copyfiles=rtl8125_files"}},
        {"rtl8125b_install.ndi.ntx86.services", {"addservice=rtl8125,0x00000002,rtl8125_service"}},
        {"rtl8125b_name", {"hkr,,friendlyname,0,\"rtl8125b\""}},
        {"rtl8125b_install.ndi.ntx86.hw", {"addreg=rtl8125b_name"}},
        {"rtl8125_install.ndi.ntx86.hw", {"addreg=rtl8125_name"}},
        {"rtl8125_name", {"hkr,,friendlyname,0,\"rtl8125\""}},
        {"rtl8125_install.ndi.ntx86", {"characteristics=0x4", "bustype=5",
            "addreg=rtl8125_ndi", "copyfiles=rtl8125_files"}},
        {"rtl8125_files", {"rtl8125.sys"}},
        {"rtl8125_ndi", {"hkr,ndi,service,0,\"rtl8125\"",
            "hkr,ndi\\interfaces,upperrange,0,\"ndis5\"",
            "hkr,ndi\\interfaces,lowerrange,0,\"ethernet\""}},
        {"rtl8125_install.ndi.ntx86.services", {"addservice=rtl8125,0x00000002,rtl8125_service"}},
        {"rtl8125_service", {"servicetype=1", "starttype=3", "errorcontrol=0",
            "servicebinary=%12%\\rtl8125.sys", "loadordergroup=ndis"}},
        {"strings", {"provider=\"rtl8125reactosdevelopment\"",
            "hardwarevendor=\"realtek\"", "device=\"rtl8125\"", "deviceb=\"rtl8125b\""}}
    };
    return sections == exact;
}

// PE32 validation bounds every file/RVA lookup. It is not a signature verifier.
class PeView {
public:
    explicit PeView(const std::vector<std::uint8_t>& input) : bytes_(input) {}

    bool valid() const
    {
        if (bytes_.size() < 256 || bytes_.size() > 16 * 1024 * 1024 ||
            word(0) != 0x5a4d) {
            return false;
        }
        const auto pe = dword(0x3c);
        if (!range(pe, 24) || dword(pe) != 0x00004550 || word(pe + 4) != 0x14c) {
            return false;
        }
        const auto sections = word(pe + 6);
        const auto optional_size = word(pe + 20);
        const auto optional = static_cast<std::size_t>(pe) + 24;
        const auto table = optional + optional_size;
        if (sections == 0 || sections > 96 || optional_size < 224 ||
            !range(optional, optional_size) || !range(table, sections * 40) ||
            word(optional) != 0x10b || word(optional + 68) != 1 ||
            (word(pe + 22) & 0x102) != 0x102 || dword(optional + 92) < 16 ||
            dword(optional + 92) > (optional_size - 96U) / 8U ||
            dword(optional + 96 + 9 * 8) != 0 || dword(optional + 96 + 14 * 8) != 0) {
            return false;
        }
        bool executable_entry{};
        const auto entry = dword(optional + 16);
        for (std::size_t index = 0; index < sections; ++index) {
            const auto record = table + index * 40;
            const auto address = dword(record + 12);
            const auto raw_size = dword(record + 16);
            const auto raw_offset = dword(record + 20);
            if (!range(raw_offset, raw_size)) {
                return false;
            }
            std::string section_name;
            for (std::size_t character = 0; character < 8 && bytes_[record + character] != 0; ++character) {
                section_name += static_cast<char>(std::tolower(bytes_[record + character]));
            }
            if (raw_size != 0 && (section_name == ".crt" || section_name == ".ctors" ||
                    section_name == ".dtors" || section_name == ".tls")) {
                return false;
            }
            if (entry >= address && entry - address < raw_size &&
                (dword(record + 36) & 0x20000000) != 0) {
                executable_entry = true;
            }
        }
        if (!executable_entry || entry == 0) {
            return false;
        }
        const auto imports = dword(optional + 104);
        const auto import_size = dword(optional + 108);
        std::size_t descriptor{};
        if (import_size < 20 || import_size > 1024 * 1024 ||
            !map_rva(imports, import_size, table, sections, descriptor)) {
            return false;
        }
        bool ndis{};
        bool kernel{};
        bool terminated{};
        for (std::size_t offset = 0; offset <= import_size - 20 && offset < 20 * 16; offset += 20) {
            bool empty = true;
            for (std::size_t index = 0; index < 20; ++index) {
                empty = empty && bytes_[descriptor + offset + index] == 0;
            }
            if (empty) {
                terminated = true;
                break;
            }
            const auto name_rva = dword(descriptor + offset + 12);
            std::string name;
            bool name_terminated{};
            for (std::size_t index = 0; index < 260; ++index) {
                std::size_t character{};
                if (name_rva > UINT32_MAX - index ||
                    !map_rva(static_cast<std::uint32_t>(name_rva + index), 1,
                        table, sections, character)) {
                    return false;
                }
                const auto byte = bytes_[character];
                if (byte == 0) {
                    name_terminated = true;
                    break;
                }
                if (byte < 0x21 || byte > 0x7e) {
                    return false;
                }
                name += static_cast<char>(std::tolower(byte));
            }
            if (!name_terminated || (name != "ndis.sys" && name != "ntoskrnl.exe" &&
                    name != "hal.dll")) {
                return false;
            }
            ndis = ndis || name == "ndis.sys";
            kernel = kernel || name == "ntoskrnl.exe";
        }
        return terminated && ndis && kernel;
    }

private:
    bool range(std::size_t offset, std::size_t count) const
    {
        return offset <= bytes_.size() && count <= bytes_.size() - offset;
    }
    std::uint16_t word(std::size_t offset) const
    {
        return range(offset, 2) ? static_cast<std::uint16_t>(bytes_[offset] |
            (static_cast<std::uint16_t>(bytes_[offset + 1]) << 8)) : 0;
    }
    std::uint32_t dword(std::size_t offset) const
    {
        return range(offset, 4) ? static_cast<std::uint32_t>(word(offset)) |
            (static_cast<std::uint32_t>(word(offset + 2)) << 16) : 0;
    }
    bool map_rva(std::uint32_t rva, std::size_t count, std::size_t table,
        std::size_t sections, std::size_t& output) const
    {
        for (std::size_t index = 0; index < sections; ++index) {
            const auto record = table + index * 40;
            const auto base = dword(record + 12);
            const auto raw_size = dword(record + 16);
            if (rva >= base && rva - base <= raw_size && count <= raw_size - (rva - base)) {
                const auto raw = dword(record + 20);
                output = static_cast<std::size_t>(raw) + (rva - base);
                return range(output, count);
            }
        }
        return false;
    }
    const std::vector<std::uint8_t>& bytes_;
};
}
