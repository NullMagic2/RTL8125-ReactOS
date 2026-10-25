/*
Module: rtl8125.installer.model
Purpose: Exact PCI identity, reboot evidence policy and bounded SHA256 for user-mode setup.
Created: 2026-10-24
Architecture: Portable policy; native backend owns actual SetupAPI and system mutations.
*/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rtl8125::installer {
enum class Card { unrelated, rtl8125, rtl8125b };
Card identify(const std::vector<std::wstring>& hardware_ids);
const wchar_t* card_name(Card card) noexcept;
bool safe_component(const std::wstring& name) noexcept;
// Resolve a basename-only boot module only through an unchanged, explicit kernel
// boot-service path. Approved file hash and earlier-boot evidence remain separate.
bool boot_ndis_identity(const std::wstring& image_path, const std::wstring& windows,
    std::uint32_t start, std::uint32_t type, std::uint64_t service_write,
    std::uint64_t boot_time);

struct CompatibilityEvidence {
    bool reactos_i386{};
    bool administrator{};
    bool loaded_path_matches{};
    bool approved_hash{};
    bool journal_hash_matches{};
    std::uint64_t boot_time{};
    std::uint64_t observed_boot_time{};
    std::uint64_t file_write_time{};
};
enum class Compatibility { unsupported_os, administrator_required, wrong_image,
    reboot_required, ready };
Compatibility compatibility(const CompatibilityEvidence& evidence) noexcept;

class Sha256 {
public:
    Sha256() noexcept;
    void update(const std::uint8_t* bytes, std::size_t count) noexcept;
    std::string finish() noexcept;
private:
    void transform() noexcept;
    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> block_{};
    std::uint64_t count_{};
    std::size_t used_{};
};
}
