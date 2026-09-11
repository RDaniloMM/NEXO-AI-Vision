// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cuajone::platform {

struct PlatformPathOverrides {
    std::filesystem::path config_dir;
    std::filesystem::path data_dir;
    std::filesystem::path state_dir;
};

struct PlatformPaths {
    std::filesystem::path executable_dir;
    std::filesystem::path config_dir;
    std::filesystem::path data_dir;
    std::filesystem::path state_dir;
    std::filesystem::path profiles_dir;
    std::filesystem::path logs_dir;
    std::vector<std::filesystem::path> installed_model_roots;
};

PlatformPaths resolvePlatformPaths(
    const std::filesystem::path& application_dir = {},
    const PlatformPathOverrides& overrides = {});

std::vector<std::filesystem::path> modelRootCandidates(
    const std::filesystem::path& configured_root = {},
    const std::filesystem::path& application_dir = {});

std::filesystem::path executableDirectory();
bool ensureWritableDirectory(const std::filesystem::path& directory);

std::wstring wideFromUtf8(std::string_view value);
std::string utf8FromWide(std::wstring_view value);
void atomicReplaceFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination);

namespace detail {
PlatformPaths defaultPlatformPaths(const std::filesystem::path& application_dir);
std::vector<std::filesystem::path> defaultModelRootCandidates(
    const std::filesystem::path& application_dir);
}

}  // namespace cuajone::platform
