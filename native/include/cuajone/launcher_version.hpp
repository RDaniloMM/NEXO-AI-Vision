// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace cuajone::launcher {
// No inferred product, installer, directory, or source-code version fallback.
std::optional<std::wstring> executableFileVersion(const std::filesystem::path& path);
std::optional<std::wstring> runningLauncherVersion();
}  // namespace cuajone::launcher
