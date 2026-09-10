// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "cuajone/inference_settings.hpp"
#include "cuajone/types.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace cuajone::launcher {

enum class AnalyticsMode {
    PpeOnly,
    PpeFall,
};

enum class ComputeMode {
    Auto,
    Cuda,
    Cpu,
};

enum class UiLanguage {
    English,
    Spanish,
};

enum class ThemeMode {
    Light,
    Dark,
};

struct OperatorPreferences {
    std::size_t schema_version{1};
    UiLanguage language{UiLanguage::English};
    ThemeMode theme{ThemeMode::Light};
    int image_size{kDefaultImageSize};
    std::array<float, kPpeOutputLabels.size()> ppe_class_confidences{
        0.10F, 0.10F, 0.10F, 0.10F, 0.10F, 0.10F, 0.10F, 0.10F,
    };
    std::array<bool, kPpeItemCount> ppe_enabled{true, true, true, true, true, true, true};
    bool show_window{true};
    RtspTransport rtsp_transport{RtspTransport::Tcp};
    VideoAcceleration video_acceleration{VideoAcceleration::Auto};
    std::wstring stream_resolution{L"1920x1080"};
    int stream_fps{25};
};

struct CameraConnectionProfile {
    std::wstring name{L"Nueva cámara"};
    std::wstring username{L"user"};
    std::wstring password{L"password"};
    std::wstring host{L"IP"};
    std::uint16_t port{554};
    std::wstring path{L"/axis-media/media.amp"};
    std::wstring resolution{L"1920x1080"};
    int fps{25};
    int compression{30};
    int maximum_bitrate_kbps{6000};
    std::wstring bitrate_mode{L"mbr"};
    std::wstring bitrate_priority{L"quality"};
    int zipstream_strength{10};
    std::wstring gop_mode{L"fixed"};
    int keyframe_interval{15};
    bool dynamic_fps{};
    bool audio{};
    RtspTransport transport{RtspTransport::Tcp};
    VideoAcceleration video_acceleration{VideoAcceleration::Auto};
};

struct CameraLaunchSource {
    std::wstring source;
    std::wstring label;
    RtspTransport transport{RtspTransport::Tcp};
    VideoAcceleration video_acceleration{VideoAcceleration::Auto};
};

struct ManagedModelSet {
    std::filesystem::path root;
    std::filesystem::path ppe_engine;
    std::filesystem::path pose_engine;
    std::filesystem::path ppe_onnx;
    std::filesystem::path pose_onnx;
    bool tensor_rt_complete{};
    bool onnx_complete{};
};

struct LauncherSettings {
    // Session-only diagnostics; operator preference schema remains unchanged.
    bool performance_report{};
    int telemetry_interval_seconds{5};
    std::wstring source;
    std::vector<CameraLaunchSource> cameras;
    std::filesystem::path output;
    AnalyticsMode analytics_mode{AnalyticsMode::PpeFall};
    ComputeMode compute_mode{ComputeMode::Auto};
    RtspTransport rtsp_transport{RtspTransport::Tcp};
    VideoAcceleration video_acceleration{VideoAcceleration::Auto};
    std::wstring stream_resolution{L"1920x1080"};
    int stream_fps{25};
    std::filesystem::path managed_model_root;
    // Dev-only fallback: when true, buildLaunchPlan() searches ordered dev
    // candidate roots (exe dir, repo tree, staged installer bundle, installed
    // location) if managed_model_root has no complete set. Default false keeps
    // installer/test paths strict.
    bool allow_dev_model_fallback{false};
    std::wstring source_label;
    std::vector<std::pair<std::wstring, std::wstring>> runtime_options;
    int image_size{kDefaultImageSize};
    std::array<float, kPpeOutputLabels.size()> ppe_class_confidences{
        0.10F, 0.10F, 0.10F, 0.10F, 0.10F, 0.10F, 0.10F, 0.10F,
    };
    std::array<bool, kPpeItemCount> ppe_enabled{true, true, true, true, true, true, true};
    bool show_window{true};
};

struct LaunchPlan {
    std::vector<std::wstring> arguments;
    bool has_cuda_candidate{};
    bool has_cpu_candidate{};
};

std::filesystem::path adjacentOnnxManifest(const std::filesystem::path& model);
ManagedModelSet resolveManagedModelSet(
    const std::filesystem::path& root,
    bool pose_required);
// Ordered dev candidate roots for a managed model bundle, starting with the
// configured root. Dev bundles keep the strict installed file names
// (ppe.onnx/pose.onnx plus adjacent manifests); raw training/export names such
// as best_ppe.onnx or yolo26s-pose.onnx are intentionally NOT accepted here.
// To produce a dev bundle, stage it with tools/export_runtime_onnx.py (used by
// installer/native/build-installer.ps1) or copy the staged
// <stage>/bin/models bundle next to the freshly built launcher.
std::vector<std::filesystem::path> managedModelRootCandidates(
    const std::filesystem::path& configured_root,
    const std::filesystem::path& exe_dir = {});
// First candidate holding a complete ONNX set (or a TensorRT-only set when no
// candidate has ONNX). Returns std::nullopt when no candidate is complete.
std::optional<ManagedModelSet> resolveBestManagedModelSet(
    const std::vector<std::filesystem::path>& candidates,
    bool pose_required);
std::wstring describeModelCandidates(
    const std::vector<std::filesystem::path>& candidates);
LaunchPlan buildLaunchPlan(const LauncherSettings& settings, bool preflight);
inline constexpr std::array<int, 5> kTelemetryIntervals{1, 5, 10, 30, 60};
inline constexpr std::array<std::wstring_view, 7> kStreamResolutions{
    L"640x360", L"640x480", L"1280x720", L"1920x1080",
    L"2560x1440", L"2688x1512", L"3840x2160",
};
inline constexpr std::array<int, 6> kStreamFrameRates{5, 10, 15, 20, 25, 30};
float parsePpeConfidenceThreshold(std::wstring_view text);
std::wstring formatPpeConfidenceThreshold(float value);
OperatorPreferences parseOperatorPreferences(std::string_view text);
std::string serializeOperatorPreferences(const OperatorPreferences& preferences);
OperatorPreferences loadOperatorPreferences(const std::filesystem::path& path) noexcept;
void saveOperatorPreferencesAtomic(
    const std::filesystem::path& path,
    const OperatorPreferences& preferences);
std::vector<std::string_view> visibleLauncherControlKeys();
std::wstring quoteWindowsArgument(std::wstring_view argument);
std::wstring buildWindowsCommandLine(const std::vector<std::wstring>& arguments);
std::string redactRtspCredentials(std::string_view text);
bool isValidSavedCameraProfileName(std::wstring_view name);
std::wstring_view savedCameraCredentialTargetPrefix();
std::wstring savedCameraCredentialTarget(std::wstring_view name);
void validateRtspCameraUrl(std::wstring_view source);
void validateCameraConnectionProfile(const CameraConnectionProfile& profile);
std::wstring buildAxisRtspUrl(const CameraConnectionProfile& profile);
std::string serializeCameraConnectionProfile(const CameraConnectionProfile& profile);
CameraConnectionProfile parseCameraConnectionProfile(
    std::string_view payload,
    std::wstring_view profile_name = {});
CameraConnectionProfile parseLegacyCameraUrl(
    std::wstring_view source,
    std::wstring_view profile_name);

}  // namespace cuajone::launcher
