// SPDX-License-Identifier: AGPL-3.0-only

#include "../platform_secrets.hpp"
#include "../platform_paths.hpp"

#include <windows.h>
#include <wincred.h>

#include <cstring>
#include <vector>

namespace cuajone::platform {
namespace {

std::wstring target(std::wstring_view profile_name) {
    return L"NexoAI Vision/Qt/RTSP/" + std::wstring(profile_name);
}

}  // namespace

SecretStoreInfo cameraPasswordStoreInfo() noexcept {
    return {true, "Windows Credential Manager"};
}

bool saveCameraPassword(std::wstring_view profile_name, std::wstring_view password) {
    if (!detail::validSecretProfileName(profile_name)) return false;
    const std::wstring name = target(profile_name);
    const std::string bytes = utf8FromWide(password);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(name.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(bytes.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) != FALSE;
}

std::optional<std::wstring> loadCameraPassword(std::wstring_view profile_name) {
    if (!detail::validSecretProfileName(profile_name)) return std::nullopt;
    const std::wstring name = target(profile_name);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return std::nullopt;
    std::string bytes;
    if (credential->CredentialBlob != nullptr && credential->CredentialBlobSize != 0) {
        bytes.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
            credential->CredentialBlobSize);
    }
    CredFree(credential);
    try { return wideFromUtf8(bytes); }
    catch (...) { return std::nullopt; }
}

void deleteCameraPassword(std::wstring_view profile_name) noexcept {
    if (!detail::validSecretProfileName(profile_name)) return;
    const std::wstring name = target(profile_name);
    CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0);
}

}  // namespace cuajone::platform
