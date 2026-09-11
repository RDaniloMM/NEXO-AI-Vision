// SPDX-License-Identifier: AGPL-3.0-only

#include "cuajone/launcher_support.hpp"
#include "platform_paths.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace cuajone::launcher {
namespace {

constexpr wchar_t kSavedCameraCredentialTargetPrefix[] = L"NexoAI Vision/RTSP/";

std::string utf8FromWide(std::wstring_view value);
bool equalsAsciiCaseInsensitive(std::wstring_view left, std::wstring_view right);
void validateStreamSettings(std::wstring_view resolution, int fps);

bool regularFile(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error) && !error;
}

bool regularFileWithExtension(
    const std::filesystem::path& path,
    std::wstring_view expected_extension) {
    if (!regularFile(path)) return false;
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return extension == expected_extension;
}

std::wstring wideFromUtf8(std::string_view value) {
    return platform::wideFromUtf8(value);
}

bool isRtspSource(std::wstring_view source) {
    return source.starts_with(L"rtsp://") || source.starts_with(L"rtsps://");
}

bool isSavedCameraProfileCharacter(wchar_t character) {
    return std::iswalnum(character) != 0 || character == L' ' || character == L'_'
        || character == L'-' || character == L'.';
}

}  // namespace

bool isValidSavedCameraProfileName(std::wstring_view name) {
    return !name.empty() && name.size() <= 80
        && std::all_of(name.begin(), name.end(), isSavedCameraProfileCharacter);
}

std::wstring_view savedCameraCredentialTargetPrefix() {
    return kSavedCameraCredentialTargetPrefix;
}

std::wstring savedCameraCredentialTarget(std::wstring_view name) {
    if (!isValidSavedCameraProfileName(name)) {
        throw std::invalid_argument("Saved camera profile name is invalid");
    }
    return std::wstring(kSavedCameraCredentialTargetPrefix) + std::wstring(name);
}

void validateRtspCameraUrl(std::wstring_view source) {
    if (!isRtspSource(source)) {
        throw std::invalid_argument("Source must be an rtsp:// or rtsps:// camera URL");
    }
    const std::size_t scheme_end = source.find(L"://");
    const std::size_t authority_start = scheme_end + 3;
    const std::size_t authority_end = source.find_first_of(L"/?#", authority_start);
    const std::size_t end = authority_end == std::wstring_view::npos ? source.size() : authority_end;
    if (authority_start >= end) {
        throw std::invalid_argument("RTSP camera URL must include a host");
    }
    const std::wstring_view authority = source.substr(authority_start, end - authority_start);
    if (std::any_of(authority.begin(), authority.end(), [](wchar_t character) {
            return std::iswspace(character) != 0 || character < 0x20;
        })) {
        throw std::invalid_argument("RTSP camera URL authority is invalid");
    }
    const std::size_t at = authority.rfind(L'@');
    const std::wstring_view host_and_port = at == std::wstring_view::npos
        ? authority : authority.substr(at + 1);
    if (host_and_port.empty()) {
        throw std::invalid_argument("RTSP camera URL must include a host");
    }
}

namespace {

std::string percentEncodeUtf8(std::wstring_view value) {
    const std::string utf8 = utf8FromWide(value);
    std::ostringstream output;
    output << std::uppercase << std::hex;
    for (const unsigned char character : utf8) {
        if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9') || character == '-' || character == '_'
            || character == '.' || character == '~') {
            output << static_cast<char>(character);
        } else {
            output << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(character);
        }
    }
    return output.str();
}

int hexDigit(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

std::wstring percentDecodeUtf8(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            decoded.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) throw std::invalid_argument("Invalid percent-encoded profile field");
        const int high = hexDigit(value[index + 1]);
        const int low = hexDigit(value[index + 2]);
        if (high < 0 || low < 0) throw std::invalid_argument("Invalid percent-encoded profile field");
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return wideFromUtf8(decoded);
}

int parseProfileInteger(std::string_view value, std::string_view name, int minimum, int maximum) {
    int parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()
        || parsed < minimum || parsed > maximum) {
        throw std::invalid_argument("Invalid camera profile field: " + std::string(name));
    }
    return parsed;
}

std::wstring queryValue(std::wstring_view url, std::wstring_view name) {
    const auto query_start = url.find(L'?');
    if (query_start == std::wstring_view::npos) return {};
    std::size_t start = query_start + 1;
    while (start <= url.size()) {
        const auto end = url.find(L'&', start);
        const auto entry = url.substr(start, end == std::wstring_view::npos ? url.size() - start : end - start);
        const auto separator = entry.find(L'=');
        if (equalsAsciiCaseInsensitive(entry.substr(0, separator), name)) {
            return separator == std::wstring_view::npos ? std::wstring{} : std::wstring(entry.substr(separator + 1));
        }
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return {};
}

}  // namespace

void validateCameraConnectionProfile(const CameraConnectionProfile& profile) {
    if (!isValidSavedCameraProfileName(profile.name)) {
        throw std::invalid_argument("Camera profile name is invalid");
    }
    if (profile.host.empty() || profile.host.find_first_of(L"/?#@ \t\r\n") != std::wstring::npos) {
        throw std::invalid_argument("Camera host is invalid");
    }
    if (profile.port == 0) throw std::invalid_argument("Camera port must be in [1, 65535]");
    if (profile.path.empty() || profile.path.front() != L'/' || profile.path.find_first_of(L"?#\r\n") != std::wstring::npos) {
        throw std::invalid_argument("Camera RTSP path must start with / and cannot contain a query");
    }
    validateStreamSettings(profile.resolution, profile.fps);
    if (profile.compression < 0 || profile.compression > 100
        || profile.maximum_bitrate_kbps < 1 || profile.maximum_bitrate_kbps > 1000000
        || profile.zipstream_strength < 0 || profile.zipstream_strength > 30
        || profile.keyframe_interval < 1 || profile.keyframe_interval > 1000) {
        throw std::invalid_argument("Camera encoding values are outside supported ranges");
    }
    if (profile.bitrate_mode != L"mbr" || profile.bitrate_priority != L"quality"
        || profile.gop_mode != L"fixed") {
        throw std::invalid_argument("Only the validated AXIS MBR/quality/fixed profile is supported");
    }
}

std::wstring buildAxisRtspUrl(const CameraConnectionProfile& profile) {
    validateCameraConnectionProfile(profile);
    std::wstring result = L"rtsp://";
    if (!profile.username.empty() || !profile.password.empty()) {
        result += wideFromUtf8(percentEncodeUtf8(profile.username));
        result += L":";
        result += wideFromUtf8(percentEncodeUtf8(profile.password));
        result += L"@";
    }
    result += profile.host + L":" + std::to_wstring(profile.port) + profile.path;
    result += L"?videocodec=h264&h264profile=high&resolution=" + profile.resolution;
    result += L"&fps=" + std::to_wstring(profile.fps);
    result += L"&audio=" + std::wstring(profile.audio ? L"1" : L"0");
    result += L"&compression=" + std::to_wstring(profile.compression);
    result += L"&videobitratemode=mbr&videomaxbitrate=" + std::to_wstring(profile.maximum_bitrate_kbps);
    result += L"&videobitratepriority=quality&videozstrength=" + std::to_wstring(profile.zipstream_strength);
    result += L"&videozgopmode=fixed&videozfpsmode=" + std::wstring(profile.dynamic_fps ? L"dynamic" : L"fixed");
    result += L"&videokeyframeinterval=" + std::to_wstring(profile.keyframe_interval);
    return result;
}

std::string serializeCameraConnectionProfile(const CameraConnectionProfile& profile) {
    validateCameraConnectionProfile(profile);
    const auto transport = profile.transport == RtspTransport::Udp ? "udp"
        : profile.transport == RtspTransport::Default ? "default" : "tcp";
    const auto acceleration = profile.video_acceleration == VideoAcceleration::D3d11 ? "d3d11"
        : profile.video_acceleration == VideoAcceleration::Vaapi ? "vaapi"
        : profile.video_acceleration == VideoAcceleration::Cpu ? "cpu" : "auto";
    std::ostringstream output;
    output << "schema_version=1\n"
           << "username=" << percentEncodeUtf8(profile.username) << '\n'
           << "password=" << percentEncodeUtf8(profile.password) << '\n'
           << "host=" << percentEncodeUtf8(profile.host) << '\n'
           << "port=" << profile.port << '\n'
           << "path=" << percentEncodeUtf8(profile.path) << '\n'
           << "resolution=" << percentEncodeUtf8(profile.resolution) << '\n'
           << "fps=" << profile.fps << '\n'
           << "compression=" << profile.compression << '\n'
           << "maximum_bitrate_kbps=" << profile.maximum_bitrate_kbps << '\n'
           << "zipstream_strength=" << profile.zipstream_strength << '\n'
           << "keyframe_interval=" << profile.keyframe_interval << '\n'
           << "dynamic_fps=" << (profile.dynamic_fps ? 1 : 0) << '\n'
           << "audio=" << (profile.audio ? 1 : 0) << '\n'
           << "transport=" << transport << '\n'
           << "video_acceleration=" << acceleration << '\n';
    return output.str();
}

CameraConnectionProfile parseCameraConnectionProfile(std::string_view payload, std::wstring_view profile_name) {
    std::map<std::string, std::string> values;
    std::istringstream input{std::string(payload)};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0
            || !values.emplace(line.substr(0, separator), line.substr(separator + 1)).second) {
            throw std::invalid_argument("Camera profile contains an invalid or duplicate field");
        }
    }
    if (values["schema_version"] != "1") throw std::invalid_argument("Unsupported camera profile schema");
    CameraConnectionProfile profile;
    if (!profile_name.empty()) profile.name = std::wstring(profile_name);
    for (const char* required : {"username", "password", "host", "port", "path", "resolution", "fps",
             "compression", "maximum_bitrate_kbps", "zipstream_strength", "keyframe_interval",
             "dynamic_fps", "audio", "transport", "video_acceleration"}) {
        if (!values.contains(required)) throw std::invalid_argument("Camera profile is missing fields");
    }
    profile.username = percentDecodeUtf8(values.at("username"));
    profile.password = percentDecodeUtf8(values.at("password"));
    profile.host = percentDecodeUtf8(values.at("host"));
    profile.port = static_cast<std::uint16_t>(parseProfileInteger(values.at("port"), "port", 1, 65535));
    profile.path = percentDecodeUtf8(values.at("path"));
    profile.resolution = percentDecodeUtf8(values.at("resolution"));
    profile.fps = parseProfileInteger(values.at("fps"), "fps", 1, 240);
    profile.compression = parseProfileInteger(values.at("compression"), "compression", 0, 100);
    profile.maximum_bitrate_kbps = parseProfileInteger(values.at("maximum_bitrate_kbps"), "maximum_bitrate_kbps", 1, 1000000);
    profile.zipstream_strength = parseProfileInteger(values.at("zipstream_strength"), "zipstream_strength", 0, 30);
    profile.keyframe_interval = parseProfileInteger(values.at("keyframe_interval"), "keyframe_interval", 1, 1000);
    profile.dynamic_fps = parseProfileInteger(values.at("dynamic_fps"), "dynamic_fps", 0, 1) != 0;
    profile.audio = parseProfileInteger(values.at("audio"), "audio", 0, 1) != 0;
    const auto& transport = values.at("transport");
    profile.transport = transport == "udp" ? RtspTransport::Udp
        : transport == "default" ? RtspTransport::Default : RtspTransport::Tcp;
    if (transport != "tcp" && transport != "udp" && transport != "default") throw std::invalid_argument("Invalid profile transport");
    const auto& acceleration = values.at("video_acceleration");
    profile.video_acceleration = acceleration == "d3d11" ? VideoAcceleration::D3d11
        : acceleration == "vaapi" ? VideoAcceleration::Vaapi
        : acceleration == "cpu" ? VideoAcceleration::Cpu : VideoAcceleration::Auto;
    if (acceleration != "auto" && acceleration != "d3d11" && acceleration != "vaapi" && acceleration != "cpu") throw std::invalid_argument("Invalid profile video acceleration");
    validateCameraConnectionProfile(profile);
    return profile;
}

CameraConnectionProfile parseLegacyCameraUrl(std::wstring_view source, std::wstring_view profile_name) {
    validateRtspCameraUrl(source);
    CameraConnectionProfile profile;
    profile.name = std::wstring(profile_name);
    const auto scheme_end = source.find(L"://") + 3;
    const auto authority_end = source.find_first_of(L"/?#", scheme_end);
    const auto authority = source.substr(scheme_end, authority_end - scheme_end);
    const auto at = authority.rfind(L'@');
    auto host_port = authority;
    if (at != std::wstring_view::npos) {
        const auto credentials = authority.substr(0, at);
        const auto colon = credentials.find(L':');
        profile.username = std::wstring(credentials.substr(0, colon));
        profile.password = colon == std::wstring_view::npos ? L"" : std::wstring(credentials.substr(colon + 1));
        host_port = authority.substr(at + 1);
    }
    const auto colon = host_port.rfind(L':');
    profile.host = std::wstring(colon == std::wstring_view::npos ? host_port : host_port.substr(0, colon));
    if (colon != std::wstring_view::npos) {
        profile.port = static_cast<std::uint16_t>(std::stoi(std::wstring(host_port.substr(colon + 1))));
    }
    const auto query_start = source.find(L'?', authority_end);
    profile.path = authority_end == std::wstring_view::npos ? L"/axis-media/media.amp"
        : std::wstring(source.substr(authority_end, (query_start == std::wstring_view::npos ? source.size() : query_start) - authority_end));
    const auto resolution = queryValue(source, L"resolution");
    if (!resolution.empty()) profile.resolution = resolution;
    const auto set_int = [&](std::wstring_view name, int& field) {
        const auto value = queryValue(source, name);
        if (!value.empty()) field = std::stoi(value);
    };
    set_int(L"fps", profile.fps);
    set_int(L"compression", profile.compression);
    set_int(L"videomaxbitrate", profile.maximum_bitrate_kbps);
    set_int(L"videozstrength", profile.zipstream_strength);
    set_int(L"videokeyframeinterval", profile.keyframe_interval);
    profile.audio = queryValue(source, L"audio") == L"1";
    profile.dynamic_fps = queryValue(source, L"videozfpsmode") == L"dynamic";
    validateCameraConnectionProfile(profile);
    return profile;
}

namespace {

bool hasNonWhitespace(std::wstring_view value) {
    return std::any_of(value.begin(), value.end(), [](wchar_t character) {
        return std::iswspace(character) == 0;
    });
}

bool equalsAsciiCaseInsensitive(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [](wchar_t lhs, wchar_t rhs) {
            return std::towlower(lhs) == std::towlower(rhs);
        });
}

void validateStreamSettings(std::wstring_view resolution, int fps) {
    if (std::ranges::find(kStreamResolutions, resolution) == kStreamResolutions.end()) {
        throw std::invalid_argument("Select a supported stream resolution");
    }
    if (std::ranges::find(kStreamFrameRates, fps) == kStreamFrameRates.end()) {
        throw std::invalid_argument("Select a supported stream frame rate");
    }
}

std::wstring upsertQueryParameter(
    std::wstring_view source,
    std::wstring_view parameter,
    std::wstring_view value) {
    const std::size_t fragment_start = source.find(L'#');
    const std::wstring_view fragment = fragment_start == std::wstring_view::npos
        ? std::wstring_view{} : source.substr(fragment_start);
    const std::wstring_view without_fragment = fragment_start == std::wstring_view::npos
        ? source : source.substr(0, fragment_start);
    const std::size_t query_start = without_fragment.find(L'?');
    const std::wstring_view base = query_start == std::wstring_view::npos
        ? without_fragment : without_fragment.substr(0, query_start);
    const std::wstring_view query = query_start == std::wstring_view::npos
        ? std::wstring_view{} : without_fragment.substr(query_start + 1);

    std::vector<std::wstring> entries;
    bool replaced = false;
    std::size_t start = 0;
    while (start <= query.size()) {
        const std::size_t end = query.find(L'&', start);
        const std::wstring_view entry = query.substr(
            start, end == std::wstring_view::npos ? query.size() - start : end - start);
        if (!entry.empty()) {
            const std::size_t equals = entry.find(L'=');
            const std::wstring_view name = entry.substr(0, equals);
            if (equalsAsciiCaseInsensitive(name, parameter)) {
                if (!replaced) {
                    entries.emplace_back(std::wstring(parameter) + L'=' + std::wstring(value));
                    replaced = true;
                }
            } else {
                entries.emplace_back(entry);
            }
        }
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    if (!replaced) entries.emplace_back(std::wstring(parameter) + L'=' + std::wstring(value));

    std::wstring result(base);
    result.push_back(L'?');
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) result.push_back(L'&');
        result += entries[index];
    }
    result += fragment;
    return result;
}

std::wstring configuredRtspSource(
    std::wstring_view source,
    std::wstring_view resolution,
    int fps) {
    std::wstring result = upsertQueryParameter(source, L"resolution", resolution);
    return upsertQueryParameter(result, L"fps", std::to_wstring(fps));
}

std::string utf8FromWide(std::wstring_view value) {
    return platform::utf8FromWide(value);
}

void appendOption(
    std::vector<std::wstring>& arguments,
    std::wstring_view option,
    const std::filesystem::path& value) {
    arguments.emplace_back(option);
    arguments.push_back(value.wstring());
}

}  // namespace

std::filesystem::path adjacentOnnxManifest(const std::filesystem::path& model) {
    std::filesystem::path result = model;
    result += L".manifest.json";
    return result;
}

ManagedModelSet resolveManagedModelSet(
    const std::filesystem::path& root,
    bool pose_required) {
    ManagedModelSet result;
    result.root = root;
    result.ppe_engine = root / L"ppe.engine";
    result.pose_engine = root / L"pose.engine";
    result.ppe_onnx = root / L"ppe.onnx";
    result.pose_onnx = root / L"pose.onnx";
    result.tensor_rt_complete = regularFileWithExtension(result.ppe_engine, L".engine")
        && (!pose_required || regularFileWithExtension(result.pose_engine, L".engine"));
    result.onnx_complete = regularFileWithExtension(result.ppe_onnx, L".onnx")
        && regularFile(adjacentOnnxManifest(result.ppe_onnx))
        && (!pose_required || (regularFileWithExtension(result.pose_onnx, L".onnx")
            && regularFile(adjacentOnnxManifest(result.pose_onnx))));
    return result;
}

namespace {

void appendCandidateOnce(
    std::vector<std::filesystem::path>& candidates,
    const std::filesystem::path& candidate) {
    if (candidate.empty()) return;
    if (std::ranges::find(candidates, candidate) != candidates.end()) return;
    candidates.push_back(candidate);
}

// Walks up from `start` looking for the repo root marker. Depth is bounded so
// a missing marker degrades to "no repo root" instead of scanning to the drive.
std::filesystem::path findRepoRoot(const std::filesystem::path& start) {
    std::filesystem::path directory = start;
    for (int depth = 0; depth < 12 && !directory.empty(); ++depth) {
        if (regularFile(directory / L"pyproject.toml")) return directory;
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return {};
}

}  // namespace

std::vector<std::filesystem::path> managedModelRootCandidates(
    const std::filesystem::path& configured_root,
    const std::filesystem::path& exe_dir) {
    std::vector<std::filesystem::path> candidates;
    appendCandidateOnce(candidates, configured_root);

    std::filesystem::path executable_dir = exe_dir;
    if (executable_dir.empty()) executable_dir = platform::executableDirectory();
    appendCandidateOnce(candidates, executable_dir);
    if (!executable_dir.empty()) {
        appendCandidateOnce(candidates, executable_dir / L"models");
    }

    std::filesystem::path repo_root = findRepoRoot(executable_dir);
    if (repo_root.empty()) {
        std::error_code error;
        repo_root = findRepoRoot(std::filesystem::current_path(error));
    }
    if (!repo_root.empty()) {
        appendCandidateOnce(candidates, repo_root);
        appendCandidateOnce(candidates, repo_root / L"models");
        appendCandidateOnce(candidates, repo_root / L"installer" / L"stage" / L"bin" / L"models");
        appendCandidateOnce(candidates, repo_root / L"installer" / L"stage" / L"models");
        appendCandidateOnce(
            candidates, repo_root / L".tools" / L"native" / L"installer" / L"stage" / L"bin" / L"models");
        appendCandidateOnce(
            candidates, repo_root / L".tools" / L"native" / L"installer" / L"stage" / L"models");
    }
    for (const auto& candidate : platform::modelRootCandidates({}, executable_dir)) {
        appendCandidateOnce(candidates, candidate);
    }
    return candidates;
}

std::optional<ManagedModelSet> resolveBestManagedModelSet(
    const std::vector<std::filesystem::path>& candidates,
    bool pose_required) {
    std::optional<ManagedModelSet> tensor_rt_only;
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        ManagedModelSet models = resolveManagedModelSet(candidate, pose_required);
        if (models.onnx_complete) return models;
        if (!tensor_rt_only.has_value() && models.tensor_rt_complete) {
            tensor_rt_only = models;
        }
    }
    return tensor_rt_only;
}

std::wstring describeModelCandidates(
    const std::vector<std::filesystem::path>& candidates) {
    std::wstring result;
    bool first = true;
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        if (!first) result += L"; ";
        first = false;
        result += candidate.wstring();
    }
    return result.empty() ? L"<no candidate paths>" : result;
}

float parsePpeConfidenceThreshold(std::wstring_view text) {
    const auto is_digit = [](wchar_t character) {
        return character >= L'0' && character <= L'9';
    };
    const std::size_t decimal = text.find(L'.');
    const std::wstring_view whole = text.substr(0, decimal);
    const std::wstring_view fraction = decimal == std::wstring_view::npos
        ? std::wstring_view{}
        : text.substr(decimal + 1);
    if (whole.empty() || !std::ranges::all_of(whole, is_digit)
        || (decimal != std::wstring_view::npos
            && (fraction.empty() || fraction.size() > 2
                || !std::ranges::all_of(fraction, is_digit)))) {
        throw std::invalid_argument("PPE class confidence must be a decimal from 0.00 to 1.00");
    }

    int whole_value{};
    for (const wchar_t character : whole) {
        whole_value = whole_value * 10 + (character - L'0');
        if (whole_value > 1) {
            throw std::invalid_argument("PPE class confidence must be a decimal from 0.00 to 1.00");
        }
    }
    int fractional_value{};
    if (!fraction.empty()) {
        fractional_value = (fraction[0] - L'0') * 10;
        if (fraction.size() == 2) fractional_value += fraction[1] - L'0';
    }
    if (whole_value == 1 && fractional_value != 0) {
        throw std::invalid_argument("PPE class confidence must be a decimal from 0.00 to 1.00");
    }
    return static_cast<float>(whole_value * 100 + fractional_value) / 100.0F;
}

std::wstring formatPpeConfidenceThreshold(float value) {
    if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
        throw std::invalid_argument("PPE class confidence must be finite and in [0, 1]");
    }
    const int hundredths = static_cast<int>(std::lround(value * 100.0F));
    if (std::abs(value - static_cast<float>(hundredths) / 100.0F) > 0.00001F) {
        throw std::invalid_argument("PPE class confidence must have at most two decimal places");
    }
    std::wstring result = std::to_wstring(hundredths / 100);
    result += L'.';
    result += static_cast<wchar_t>(L'0' + hundredths % 100 / 10);
    result += static_cast<wchar_t>(L'0' + hundredths % 10);
    return result;
}

LaunchPlan buildLaunchPlan(const LauncherSettings& settings, bool preflight) {
    if (std::ranges::find(kTelemetryIntervals, settings.telemetry_interval_seconds)
        == kTelemetryIntervals.end()) {
        throw std::invalid_argument("Telemetry interval must be 1, 5, 10, 30, or 60 seconds");
    }
    validateStreamSettings(settings.stream_resolution, settings.stream_fps);
    std::vector<CameraLaunchSource> cameras = settings.cameras;
    if (cameras.empty() && !settings.source.empty()) {
        cameras.push_back({
            isRtspSource(settings.source)
                ? configuredRtspSource(settings.source, settings.stream_resolution, settings.stream_fps)
                : settings.source,
            settings.source_label,
            settings.rtsp_transport,
            settings.video_acceleration,
        });
    }
    if (cameras.empty()) {
        throw std::invalid_argument("Camera URL or video file is required");
    }
    std::vector<std::wstring> labels;
    for (const auto& camera : cameras) {
        if (isRtspSource(camera.source)) validateRtspCameraUrl(camera.source);
        else if (!regularFile(camera.source)) {
            throw std::invalid_argument("Every source must be a valid RTSP URL or an existing video file");
        }
        if (hasNonWhitespace(camera.label) && std::ranges::find(labels, camera.label) != labels.end()) {
            throw std::invalid_argument("Selected camera names must be unique");
        }
        if (hasNonWhitespace(camera.label)) labels.push_back(camera.label);
    }
    if (settings.output.empty()) {
        throw std::invalid_argument("Output folder is required");
    }
    std::error_code output_error;
    if (std::filesystem::exists(settings.output, output_error)
        && !std::filesystem::is_directory(settings.output, output_error)) {
        throw std::invalid_argument("Output path must be a folder");
    }

    validateImageSize(settings.image_size);
    validatePpeClassConfidences(settings.ppe_class_confidences);
    if (settings.managed_model_root.empty()) {
        throw std::invalid_argument("Managed model location is required");
    }
    for (const auto& [option, value] : settings.runtime_options) {
        static_cast<void>(value);
        if (option == L"--performance-report" || option == L"--telemetry-interval-sec") {
            throw std::invalid_argument("Use the launcher menu to configure performance debugging");
        }
        if (option == L"--rtsp-transport" || option == L"--video-acceleration") {
            throw std::invalid_argument("Use the launcher controls to configure RTSP transport and video decoding");
        }
        if (option == L"--ppe-engine" || option == L"--pose-engine"
            || option == L"--ppe-onnx" || option == L"--pose-onnx"
            || option == L"--ppe-labels") {
            throw std::invalid_argument("Model paths and labels cannot be supplied through launcher options");
        }
    }

    const bool needs_pose = settings.analytics_mode == AnalyticsMode::PpeFall;
    ManagedModelSet models = resolveManagedModelSet(settings.managed_model_root, needs_pose);
    std::vector<std::filesystem::path> tried_roots{settings.managed_model_root};
    if (settings.allow_dev_model_fallback) {
        tried_roots = managedModelRootCandidates(settings.managed_model_root);
        const auto direct_onnx = models.onnx_complete;
        const auto direct_cuda = models.tensor_rt_complete || models.onnx_complete;
        const bool direct_satisfies =
            (settings.compute_mode == ComputeMode::Cpu && direct_onnx)
            || (settings.compute_mode != ComputeMode::Cpu && direct_cuda);
        if (!direct_satisfies) {
            if (const auto best = resolveBestManagedModelSet(tried_roots, needs_pose)) {
                models = *best;
            }
        }
    }
    const bool tensor_rt_candidate = models.tensor_rt_complete;
    const bool cpu_candidate = models.onnx_complete;

    const bool cuda_candidate = tensor_rt_candidate || cpu_candidate;
    const auto missingSetMessage = [&](std::wstring_view prefix) {
        std::wstring message(prefix);
        message += describeModelCandidates(tried_roots);
        return message;
    };
    if (settings.compute_mode == ComputeMode::Cuda && !cuda_candidate) {
        throw std::invalid_argument(
            utf8FromWide(missingSetMessage(L"The complete managed CUDA model set is missing. Tried: ")));
    }
    if (settings.compute_mode == ComputeMode::Cpu && !cpu_candidate) {
        throw std::invalid_argument(
            utf8FromWide(missingSetMessage(L"The complete managed ONNX model set is missing. Tried: ")));
    }
    if (settings.compute_mode == ComputeMode::Auto && !cuda_candidate && !cpu_candidate) {
        throw std::invalid_argument(
            utf8FromWide(missingSetMessage(L"No complete managed model set was found. Tried: ")));
    }

    LaunchPlan result;
    result.has_cuda_candidate = cuda_candidate;
    result.has_cpu_candidate = cpu_candidate;
    if (preflight) result.arguments.emplace_back(L"--preflight");
    for (const auto& camera : cameras) {
        result.arguments.emplace_back(L"--source");
        result.arguments.push_back(camera.source);
        if (hasNonWhitespace(camera.label)) {
            result.arguments.emplace_back(L"--source-label");
            result.arguments.push_back(camera.label);
        }
        result.arguments.emplace_back(L"--source-rtsp-transport");
        result.arguments.emplace_back(camera.transport == RtspTransport::Udp ? L"udp"
            : camera.transport == RtspTransport::Default ? L"default" : L"tcp");
        result.arguments.emplace_back(L"--source-video-acceleration");
        result.arguments.emplace_back(camera.video_acceleration == VideoAcceleration::D3d11 ? L"d3d11"
            : camera.video_acceleration == VideoAcceleration::Vaapi ? L"vaapi"
            : camera.video_acceleration == VideoAcceleration::Cpu ? L"cpu" : L"auto");
    }
    appendOption(result.arguments, L"--output", settings.output);
    result.arguments.emplace_back(L"--mode");
    result.arguments.emplace_back(needs_pose ? L"ppe-fall" : L"ppe-only");
    result.arguments.emplace_back(L"--compute");
    switch (settings.compute_mode) {
        case ComputeMode::Auto: result.arguments.emplace_back(L"auto"); break;
        case ComputeMode::Cuda: result.arguments.emplace_back(L"cuda"); break;
        case ComputeMode::Cpu: result.arguments.emplace_back(L"cpu"); break;
    }
    result.arguments.emplace_back(L"--rtsp-transport");
    switch (settings.rtsp_transport) {
        case RtspTransport::Default: result.arguments.emplace_back(L"default"); break;
        case RtspTransport::Tcp: result.arguments.emplace_back(L"tcp"); break;
        case RtspTransport::Udp: result.arguments.emplace_back(L"udp"); break;
    }
    result.arguments.emplace_back(L"--video-acceleration");
    switch (settings.video_acceleration) {
        case VideoAcceleration::Auto: result.arguments.emplace_back(L"auto"); break;
        case VideoAcceleration::D3d11: result.arguments.emplace_back(L"d3d11"); break;
        case VideoAcceleration::Vaapi: result.arguments.emplace_back(L"vaapi"); break;
        case VideoAcceleration::Cpu: result.arguments.emplace_back(L"cpu"); break;
    }
    result.arguments.emplace_back(L"--imgsz");
    result.arguments.push_back(std::to_wstring(settings.image_size));
    for (std::size_t index = 0; index < kPpeOutputLabels.size(); ++index) {
        result.arguments.emplace_back(L"--ppe-class-conf");
        result.arguments.push_back(
            wideFromUtf8(kPpeOutputLabels[index]) + L'='
            + formatPpeConfidenceThreshold(settings.ppe_class_confidences[index]));
    }
    constexpr std::array<std::size_t, kPpeItemCount> item_class_ids{0, 2, 3, 4, 5, 6, 7};
    for (std::size_t index = 0; index < item_class_ids.size(); ++index) {
        result.arguments.emplace_back(L"--ppe-enabled");
        result.arguments.push_back(wideFromUtf8(kPpeOutputLabels[item_class_ids[index]])
            + L'=' + (settings.ppe_enabled[index] ? L"1" : L"0"));
    }

    const bool include_tensor_rt = tensor_rt_candidate && settings.compute_mode != ComputeMode::Cpu;
    const bool include_onnx = cpu_candidate;
    if (include_tensor_rt) {
        appendOption(result.arguments, L"--ppe-engine", models.ppe_engine);
        if (needs_pose) appendOption(result.arguments, L"--pose-engine", models.pose_engine);
    }
    if (include_onnx) {
        appendOption(result.arguments, L"--ppe-onnx", models.ppe_onnx);
        if (needs_pose) appendOption(result.arguments, L"--pose-onnx", models.pose_onnx);
    }
    result.arguments.emplace_back(L"--ppe-labels");
    result.arguments.emplace_back(
        L"Gloves,Person,Safety_boots,Vest,respirador,tapaorejas,Hard_hat,lentes_protectores");
    for (const auto& [option, value] : settings.runtime_options) {
        if (!option.empty() && hasNonWhitespace(value)) {
            result.arguments.push_back(option);
            result.arguments.push_back(value);
        }
    }
    if (settings.show_window) result.arguments.emplace_back(L"--show");
    // Preflight accepts the same configuration but does not run capture.
    if (settings.performance_report) {
        result.arguments.emplace_back(L"--performance-report");
        result.arguments.emplace_back(L"--telemetry-interval-sec");
        result.arguments.push_back(std::to_wstring(settings.telemetry_interval_seconds));
    }
    return result;
}

OperatorPreferences parseOperatorPreferences(std::string_view text) {
    std::map<std::string, std::string> values;
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0
            || !values.emplace(line.substr(0, separator), line.substr(separator + 1)).second) {
            throw std::invalid_argument("Preferences contain an invalid or duplicate entry");
        }
    }
    static constexpr std::array<std::string_view, 11> supported_keys{
        "schema_version", "language", "theme", "imgsz", "ppe_class_conf",
        "show_window", "ppe_enabled", "rtsp_transport", "video_acceleration",
        "stream_resolution", "stream_fps",
    };
    const bool has_unsupported_key = std::ranges::any_of(values, [](const auto& entry) {
        return std::ranges::find(supported_keys, entry.first) == supported_keys.end();
    });
    if (has_unsupported_key || !values.contains("schema_version")
        || !values.contains("language") || !values.contains("theme")
        || !values.contains("imgsz") || !values.contains("ppe_class_conf")) {
        throw std::invalid_argument("Preferences contain missing or unsupported entries");
    }
    OperatorPreferences result;
    if (values.at("schema_version") != "1") {
        throw std::invalid_argument("Unsupported preferences schema_version");
    }
    if (values.at("language") == "en") result.language = UiLanguage::English;
    else if (values.at("language") == "es") result.language = UiLanguage::Spanish;
    else throw std::invalid_argument("Preferences language must be en or es");
    if (values.at("theme") == "light") result.theme = ThemeMode::Light;
    else if (values.at("theme") == "dark") result.theme = ThemeMode::Dark;
    else throw std::invalid_argument("Preferences theme must be light or dark");
    const auto& image_text = values.at("imgsz");
    const auto image_result = std::from_chars(
        image_text.data(), image_text.data() + image_text.size(), result.image_size);
    if (image_result.ec != std::errc{} || image_result.ptr != image_text.data() + image_text.size()) {
        throw std::invalid_argument("Preferences imgsz is invalid");
    }
    validateImageSize(result.image_size);

    std::istringstream thresholds(values.at("ppe_class_conf"));
    std::string entry;
    std::size_t index = 0;
    while (std::getline(thresholds, entry, ',')) {
        if (index >= kPpeOutputLabels.size()) {
            throw std::invalid_argument("Preferences contain too many PPE thresholds");
        }
        const auto separator = entry.find(':');
        if (separator == std::string::npos
            || entry.substr(0, separator) != kPpeOutputLabels[index]) {
            throw std::invalid_argument("Preferences PPE threshold order is invalid");
        }
        const std::string_view number(entry.data() + separator + 1, entry.size() - separator - 1);
        try {
            result.ppe_class_confidences[index] = parsePpeConfidenceThreshold(
                std::wstring(number.begin(), number.end()));
        } catch (const std::invalid_argument&) {
            throw std::invalid_argument("Preferences PPE threshold value is invalid");
        }
        ++index;
    }
    if (index != kPpeOutputLabels.size()) {
        throw std::invalid_argument("Preferences require exactly eight PPE thresholds");
    }
    validatePpeClassConfidences(result.ppe_class_confidences);
    if (const auto show_window = values.find("show_window"); show_window != values.end()) {
        if (show_window->second == "1") result.show_window = true;
        else if (show_window->second == "0") result.show_window = false;
        else throw std::invalid_argument("Preferences show_window must be 0 or 1");
    }
    if (const auto enabled = values.find("ppe_enabled"); enabled != values.end()) {
        std::istringstream switches(enabled->second);
        std::string switch_entry;
        constexpr std::array<std::size_t, kPpeItemCount> item_class_ids{0, 2, 3, 4, 5, 6, 7};
        for (std::size_t switch_index = 0; switch_index < item_class_ids.size(); ++switch_index) {
            const std::size_t class_id = item_class_ids[switch_index];
            if (!std::getline(switches, switch_entry, ',')) throw std::invalid_argument("Preferences PPE switches are incomplete");
            const auto separator = switch_entry.find(':');
            if (separator == std::string::npos || switch_entry.substr(0, separator) != kPpeOutputLabels[class_id]
                || (switch_entry.substr(separator + 1) != "0" && switch_entry.substr(separator + 1) != "1")) {
                throw std::invalid_argument("Preferences PPE switch order or value is invalid");
            }
            result.ppe_enabled[switch_index] = switch_entry.substr(separator + 1) == "1";
        }
        if (std::getline(switches, switch_entry, ',')) throw std::invalid_argument("Preferences PPE switches are excessive");
    }
    if (const auto transport = values.find("rtsp_transport"); transport != values.end()) {
        if (transport->second == "default") result.rtsp_transport = RtspTransport::Default;
        else if (transport->second == "tcp") result.rtsp_transport = RtspTransport::Tcp;
        else if (transport->second == "udp") result.rtsp_transport = RtspTransport::Udp;
        else throw std::invalid_argument("Preferences rtsp_transport must be default, tcp, or udp");
    }
    if (const auto acceleration = values.find("video_acceleration"); acceleration != values.end()) {
        if (acceleration->second == "auto") result.video_acceleration = VideoAcceleration::Auto;
        else if (acceleration->second == "d3d11") result.video_acceleration = VideoAcceleration::D3d11;
        else if (acceleration->second == "vaapi") result.video_acceleration = VideoAcceleration::Vaapi;
        else if (acceleration->second == "cpu") result.video_acceleration = VideoAcceleration::Cpu;
        else throw std::invalid_argument("Preferences video_acceleration must be auto, d3d11, vaapi, or cpu");
    }
    if (const auto resolution = values.find("stream_resolution"); resolution != values.end()) {
        result.stream_resolution = wideFromUtf8(resolution->second);
    }
    if (const auto fps = values.find("stream_fps"); fps != values.end()) {
        const auto parsed = std::from_chars(
            fps->second.data(), fps->second.data() + fps->second.size(), result.stream_fps);
        if (parsed.ec != std::errc{} || parsed.ptr != fps->second.data() + fps->second.size()) {
            throw std::invalid_argument("Preferences stream_fps is invalid");
        }
    }
    validateStreamSettings(result.stream_resolution, result.stream_fps);
    return result;
}

std::string serializeOperatorPreferences(const OperatorPreferences& preferences) {
    if (preferences.schema_version != 1) {
        throw std::invalid_argument("Unsupported preferences schema_version");
    }
    validateImageSize(preferences.image_size);
    validatePpeClassConfidences(preferences.ppe_class_confidences);
    validateStreamSettings(preferences.stream_resolution, preferences.stream_fps);
    std::ostringstream output;
    output << "schema_version=1\n"
           << "language=" << (preferences.language == UiLanguage::Spanish ? "es" : "en") << '\n'
           << "theme=" << (preferences.theme == ThemeMode::Dark ? "dark" : "light") << '\n'
           << "imgsz=" << preferences.image_size << '\n'
           << "show_window=" << (preferences.show_window ? "1" : "0") << '\n'
           << "rtsp_transport="
           << (preferences.rtsp_transport == RtspTransport::Tcp ? "tcp"
               : preferences.rtsp_transport == RtspTransport::Udp ? "udp" : "default") << '\n'
           << "video_acceleration="
            << (preferences.video_acceleration == VideoAcceleration::D3d11 ? "d3d11"
                : preferences.video_acceleration == VideoAcceleration::Vaapi ? "vaapi"
                : preferences.video_acceleration == VideoAcceleration::Cpu ? "cpu" : "auto") << '\n'
           << "stream_resolution="
           << utf8FromWide(preferences.stream_resolution) << '\n'
           << "stream_fps=" << preferences.stream_fps << '\n'
           << "ppe_class_conf=";
    for (std::size_t index = 0; index < kPpeOutputLabels.size(); ++index) {
        if (index != 0) output << ',';
        const std::wstring threshold = formatPpeConfidenceThreshold(
            preferences.ppe_class_confidences[index]);
        output << kPpeOutputLabels[index] << ':'
               << utf8FromWide(threshold);
    }
    output << '\n';
    output << "ppe_enabled=";
    constexpr std::array<std::size_t, kPpeItemCount> item_class_ids{0, 2, 3, 4, 5, 6, 7};
    for (std::size_t index = 0; index < item_class_ids.size(); ++index) {
        if (index != 0) output << ',';
        output << kPpeOutputLabels[item_class_ids[index]] << ':'
               << (preferences.ppe_enabled[index] ? '1' : '0');
    }
    output << '\n';
    return output.str();
}

OperatorPreferences loadOperatorPreferences(const std::filesystem::path& path) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        const std::string text{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (text.size() > 16U * 1024U) return {};
        return parseOperatorPreferences(text);
    } catch (...) {
        return {};
    }
}

void saveOperatorPreferencesAtomic(
    const std::filesystem::path& path,
    const OperatorPreferences& preferences) {
    const std::string text = serializeOperatorPreferences(preferences);
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) throw std::runtime_error("Could not write operator preferences");
    }
    platform::atomicReplaceFile(temporary, path);
}

std::vector<std::string_view> visibleLauncherControlKeys() {
    return {
        "camera_profile_list", "camera_profile_new", "camera_profile_edit",
        "camera_profile_delete", "camera_profile_select_all", "video_file", "output",
        "ppe_profile_modal", "advanced_settings_modal", "language_icon", "theme_icon",
        "validate", "start", "stop", "status", "log_path",
    };
}

std::wstring quoteWindowsArgument(std::wstring_view argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring result(1, L'\"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring buildWindowsCommandLine(const std::vector<std::wstring>& arguments) {
    std::wstring result;
    for (const auto& argument : arguments) {
        if (!result.empty()) result.push_back(L' ');
        result += quoteWindowsArgument(argument);
    }
    return result;
}

std::string redactRtspCredentials(std::string_view text) {
    std::string result;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t rtsp = text.find("rtsp://", position);
        const std::size_t rtsps = text.find("rtsps://", position);
        const std::size_t scheme = rtsp == std::string_view::npos
            ? rtsps
            : (rtsps == std::string_view::npos ? rtsp : std::min(rtsp, rtsps));
        if (scheme == std::string_view::npos) {
            result.append(text.substr(position));
            break;
        }
        result.append(text.substr(position, scheme - position));
        const std::size_t scheme_end = text.find("://", scheme) + 3;
        const std::size_t authority_end = text.find_first_of("/?# \t\r\n", scheme_end);
        const std::size_t end = authority_end == std::string_view::npos
            ? text.size() : authority_end;
        const std::size_t at = text.substr(scheme_end, end - scheme_end).rfind('@');
        if (at == std::string_view::npos) {
            result.append(text.substr(scheme, end - scheme));
        } else {
            result.append(text.substr(scheme, scheme_end - scheme));
            result += "***@";
            result.append(text.substr(scheme_end + at + 1, end - scheme_end - at - 1));
        }
        position = end;
    }
    return result;
}

}  // namespace cuajone::launcher
