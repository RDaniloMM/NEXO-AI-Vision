// SPDX-License-Identifier: AGPL-3.0-only

#include "../platform_secrets.hpp"

#include <map>
#include <mutex>

namespace cuajone::platform {
namespace {

std::mutex mutex;
std::map<std::wstring, std::wstring> session_passwords;

}  // namespace

SecretStoreInfo cameraPasswordStoreInfo() noexcept {
    return {false, "session-only storage (libsecret is not available in this build)"};
}

bool saveCameraPassword(std::wstring_view profile_name, std::wstring_view password) {
    if (!detail::validSecretProfileName(profile_name)) return false;
    std::lock_guard lock(mutex);
    session_passwords[std::wstring(profile_name)] = std::wstring(password);
    return true;
}

std::optional<std::wstring> loadCameraPassword(std::wstring_view profile_name) {
    std::lock_guard lock(mutex);
    const auto found = session_passwords.find(std::wstring(profile_name));
    return found == session_passwords.end()
        ? std::nullopt : std::optional<std::wstring>(found->second);
}

void deleteCameraPassword(std::wstring_view profile_name) noexcept {
    std::lock_guard lock(mutex);
    session_passwords.erase(std::wstring(profile_name));
}

}  // namespace cuajone::platform
