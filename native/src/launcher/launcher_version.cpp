// SPDX-License-Identifier: AGPL-3.0-only
#include "cuajone/launcher_version.hpp"

#include <windows.h>
#include <winver.h>
#include <vector>

namespace cuajone::launcher {
std::optional<std::wstring> executableFileVersion(const std::filesystem::path& path) {
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return std::nullopt;
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return std::nullopt;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length{};
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length)
        || info == nullptr || length < sizeof(*info) || info->dwSignature != 0xFEEF04BD) {
        return std::nullopt;
    }
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
