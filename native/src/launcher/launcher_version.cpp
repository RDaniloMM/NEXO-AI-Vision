// SPDX-License-Identifier: AGPL-3.0-only
#include "cuajone/launcher_version.hpp"

#include <windows.h>
#include <winver.h>
#include <cwchar>
#include <string>
#include <vector>

namespace cuajone::launcher {
std::optional<std::wstring> executableFileVersion(const std::filesystem::path& path) {
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return std::nullopt;
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return std::nullopt;

    auto productVersion = [&data]() -> std::optional<std::wstring> {
        struct Translation {
            WORD language;
            WORD code_page;
        };
        void* raw_translations = nullptr;
        UINT translation_bytes{};
        if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &raw_translations,
                &translation_bytes) || raw_translations == nullptr
            || translation_bytes < sizeof(Translation)) {
            return std::nullopt;
        }
        const auto* translations = static_cast<const Translation*>(raw_translations);
        const auto count = translation_bytes / sizeof(Translation);
        for (UINT index = 0; index < count; ++index) {
            wchar_t sub_block[64]{};
            std::swprintf(sub_block, sizeof(sub_block) / sizeof(sub_block[0]),
                L"\\StringFileInfo\\%04x%04x\\ProductVersion",
                translations[index].language, translations[index].code_page);
            void* raw_value = nullptr;
            UINT value_length{};
            if (!VerQueryValueW(data.data(), sub_block, &raw_value, &value_length)
                || raw_value == nullptr || value_length <= 1 || value_length > 32768) {
                continue;
            }
            const auto* value = static_cast<const wchar_t*>(raw_value);
            std::wstring result(value, value_length);
            if (!result.empty() && result.back() == L'\0') result.pop_back();
            if (!result.empty()) return result;
        }
        return std::nullopt;
    };

    VS_FIXEDFILEINFO* info = nullptr;
    UINT length{};
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length)
        || info == nullptr || length < sizeof(*info) || info->dwSignature != 0xFEEF04BD) {
        return std::nullopt;
    }
    if (const auto product_version = productVersion()) return product_version;
    const bool development = (info->dwFileFlagsMask & info->dwFileFlags & VS_FF_PRIVATEBUILD) != 0;
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"."
        + std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"."
        + std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"."
        + std::to_wstring(LOWORD(info->dwFileVersionLS)) + (development ? L"-dev" : L"");
}

std::optional<std::wstring> runningLauncherVersion() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return std::nullopt;
    path.resize(length);
    return executableFileVersion(path);
}
}  // namespace cuajone::launcher
