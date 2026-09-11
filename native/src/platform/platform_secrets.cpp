// SPDX-License-Identifier: AGPL-3.0-only

#include "platform_secrets.hpp"

#include <cwctype>

namespace cuajone::platform::detail {

bool validSecretProfileName(std::wstring_view profile_name) noexcept {
    if (profile_name.empty() || profile_name.size() > 80) return false;
    for (const wchar_t character : profile_name) {
        if (std::iswalnum(character) == 0 && character != L' ' && character != L'_'
            && character != L'-' && character != L'.') return false;
    }
    return true;
}

}  // namespace cuajone::platform::detail
