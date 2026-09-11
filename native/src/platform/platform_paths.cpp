// SPDX-License-Identifier: AGPL-3.0-only

#include "platform_paths.hpp"

#include <chrono>
#include <fstream>

namespace cuajone::platform {
namespace {

void appendOnce(
    std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& candidate) {
    if (candidate.empty()) return;
    for (const auto& path : paths) {
        if (path == candidate) return;
    }
    paths.push_back(candidate);
}

}  // namespace

PlatformPaths resolvePlatformPaths(
    const std::filesystem::path& application_dir,
    const PlatformPathOverrides& overrides) {
    PlatformPaths result = detail::defaultPlatformPaths(
        application_dir.empty() ? executableDirectory() : application_dir);
    if (!overrides.config_dir.empty()) result.config_dir = overrides.config_dir;
    if (!overrides.data_dir.empty()) result.data_dir = overrides.data_dir;
    if (!overrides.state_dir.empty()) result.state_dir = overrides.state_dir;
    result.profiles_dir = result.config_dir / "profiles";
    result.logs_dir = result.data_dir / "logs";
    return result;
}

std::vector<std::filesystem::path> modelRootCandidates(
    const std::filesystem::path& configured_root,
    const std::filesystem::path& application_dir) {
    const auto executable_dir = application_dir.empty() ? executableDirectory() : application_dir;
    std::vector<std::filesystem::path> result;
    appendOnce(result, configured_root);
    appendOnce(result, executable_dir);
    if (!executable_dir.empty()) appendOnce(result, executable_dir / "models");
    for (const auto& candidate : detail::defaultModelRootCandidates(executable_dir)) {
        appendOnce(result, candidate);
    }
    return result;
}

bool ensureWritableDirectory(const std::filesystem::path& directory) {
    if (directory.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error) || error) return false;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto probe = directory / (".write-test-" + std::to_string(stamp));
    std::ofstream output(probe, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << '1';
    output.close();
    std::filesystem::remove(probe, error);
    return true;
}

}  // namespace cuajone::platform
