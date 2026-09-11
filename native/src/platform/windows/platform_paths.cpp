// SPDX-License-Identifier: AGPL-3.0-only

#include "../platform_paths.hpp"

#define NOMINMAX
#include <shlobj.h>
#include <windows.h>

#include <stdexcept>

namespace cuajone::platform {
namespace {

std::filesystem::path knownFolder(REFKNOWNFOLDERID folder) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result) || raw == nullptr) {
        throw std::runtime_error("Could not resolve a Windows known folder");
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

}  // namespace

PlatformPaths detail::defaultPlatformPaths(const std::filesystem::path& application_dir) {
    const auto local = knownFolder(FOLDERID_LocalAppData) / L"NexoAI Vision";
    const auto program = knownFolder(FOLDERID_ProgramData) / L"NexoAI Vision";
    PlatformPaths result;
    result.executable_dir = application_dir;
    result.config_dir = local;
    result.data_dir = local;
    result.state_dir = local;
    result.installed_model_roots = {
        program / L"runtime" / L"models",
        program / L"models",
    };
    return result;
}

std::vector<std::filesystem::path> detail::defaultModelRootCandidates(
    const std::filesystem::path& application_dir) {
    return detail::defaultPlatformPaths(application_dir).installed_model_roots;
}

std::filesystem::path executableDirectory() {
    std::wstring module_path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || length == static_cast<DWORD>(module_path.size())) return {};
    module_path.resize(length);
    return std::filesystem::path(module_path).parent_path();
}

std::wstring wideFromUtf8(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) throw std::invalid_argument("Value is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required) <= 0) {
        throw std::invalid_argument("Value is not valid UTF-8");
    }
    return result;
}

std::string utf8FromWide(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::invalid_argument("Value is not valid UTF-16");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) <= 0) {
        throw std::invalid_argument("Value is not valid UTF-16");
    }
    return result;
}

void atomicReplaceFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Could not atomically replace file");
    }
}

}  // namespace cuajone::platform
