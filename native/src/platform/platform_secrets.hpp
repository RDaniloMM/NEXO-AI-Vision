// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cuajone::platform {

struct SecretStoreInfo {
    bool persistent{};
    std::string_view description;
};

SecretStoreInfo cameraPasswordStoreInfo() noexcept;
bool saveCameraPassword(std::wstring_view profile_name, std::wstring_view password);
std::optional<std::wstring> loadCameraPassword(std::wstring_view profile_name);
void deleteCameraPassword(std::wstring_view profile_name) noexcept;

namespace detail {
bool validSecretProfileName(std::wstring_view profile_name) noexcept;
}

}  // namespace cuajone::platform
