// SPDX-License-Identifier: AGPL-3.0-only

#include "../platform_paths.hpp"

#include <cstdlib>
#include <stdexcept>

namespace cuajone::platform {
namespace {

std::filesystem::path environmentPath(const char* name, const std::filesystem::path& fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : std::filesystem::path(value);
}

}  // namespace

PlatformPaths detail::defaultPlatformPaths(const std::filesystem::path& application_dir) {
    const auto home = environmentPath("HOME", std::filesystem::path{});
    const auto config_base = environmentPath("XDG_CONFIG_HOME", home / ".config");
    const auto data_base = environmentPath("XDG_DATA_HOME", home / ".local" / "share");
    const auto state_base = environmentPath("XDG_STATE_HOME", home / ".local" / "state");
    PlatformPaths result;
    result.executable_dir = application_dir;
    result.config_dir = config_base / "nexoai-vision";
    result.data_dir = data_base / "nexoai-vision";
    result.state_dir = state_base / "nexoai-vision";
    result.installed_model_roots = {
        "/var/lib/nexoai-vision/models",
        "/usr/share/nexoai-vision/models",
        result.data_dir / "models",
    };
    return result;
}

std::vector<std::filesystem::path> detail::defaultModelRootCandidates(
    const std::filesystem::path& application_dir) {
    return detail::defaultPlatformPaths(application_dir).installed_model_roots;
}

std::filesystem::path executableDirectory() {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    return error || executable.empty() ? std::filesystem::path{} : executable.parent_path();
}

std::wstring wideFromUtf8(std::string_view value) {
    if (value.empty()) return {};
    std::wstring result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto byte = static_cast<unsigned char>(value[index]);
        char32_t code_point{};
        std::size_t length{};
        if (byte < 0x80) { code_point = byte; length = 1; }
        else if ((byte & 0xE0) == 0xC0) { code_point = byte & 0x1F; length = 2; }
        else if ((byte & 0xF0) == 0xE0) { code_point = byte & 0x0F; length = 3; }
        else if ((byte & 0xF8) == 0xF0) { code_point = byte & 0x07; length = 4; }
        else throw std::invalid_argument("Value is not valid UTF-8");
        if (index + length > value.size()) throw std::invalid_argument("Value is not valid UTF-8");
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xC0) != 0x80) throw std::invalid_argument("Value is not valid UTF-8");
            code_point = (code_point << 6) | (continuation & 0x3F);
        }
        if ((length == 2 && code_point < 0x80) || (length == 3 && code_point < 0x800)
            || (length == 4 && code_point < 0x10000) || code_point > 0x10FFFF
            || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            throw std::invalid_argument("Value is not valid UTF-8");
        }
        result.push_back(static_cast<wchar_t>(code_point));
        index += length;
    }
    return result;
}

std::string utf8FromWide(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        const auto code_point = static_cast<char32_t>(character);
        if (code_point < 0x80) result.push_back(static_cast<char>(code_point));
        else if (code_point < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            result.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point <= 0x10FFFF) {
            result.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else throw std::invalid_argument("Value is not valid UTF-32");
    }
    return result;
}

void atomicReplaceFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("Could not atomically replace file");
    }
}

}  // namespace cuajone::platform
