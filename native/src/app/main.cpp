// SPDX-License-Identifier: AGPL-3.0-only

#include "cuajone/capture.hpp"
#include "cuajone/cli.hpp"
#include "cuajone/engine_pipeline.hpp"
#include "cuajone/evidence.hpp"
#include "cuajone/performance_telemetry.hpp"
#include "cuajone/runtime_execution_plan.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace cuajone;
using Clock = std::chrono::steady_clock;

std::atomic_bool stop_requested{};
constexpr char kLiveAnalyticsWindowTitle[] = "NexoAI Vision - Live Analytics";

#ifdef _WIN32
constexpr wchar_t kCudaWarmupChildEnvironment[] = L"CUAJONE_INTERNAL_CUDA_WARMUP_CHILD";

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~UniqueHandle() { if (value_ != nullptr) CloseHandle(value_); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_;
};

bool isCudaWarmupChild() {
    return GetEnvironmentVariableW(kCudaWarmupChildEnvironment, nullptr, 0) != 0;
}

std::string readAllFromHandle(HANDLE handle) {
    std::string output;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        output.append(buffer, read);
    }
    return output;
}

std::string lastChildOutputLines(const std::string& output, std::size_t maximum = 2000) {
    std::string trimmed = output;
    while (!trimmed.empty()
        && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' '
            || trimmed.back() == '\t')) {
        trimmed.pop_back();
    }
    if (trimmed.size() > maximum) trimmed = trimmed.substr(trimmed.size() - maximum);
    return trimmed;
}

void runIsolatedCudaWarmup() {
    std::vector<wchar_t> executable(32768);
    const DWORD executable_size = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (executable_size == 0 || executable_size >= executable.size()) {
        throw std::runtime_error("Could not resolve the runtime executable for CUDA warmup");
    }
    executable.resize(executable_size + 1);
    std::vector<wchar_t> command_line(
        GetCommandLineW(), GetCommandLineW() + std::wcslen(GetCommandLineW()) + 1);

    if (!SetEnvironmentVariableW(kCudaWarmupChildEnvironment, L"1")) {
        throw std::runtime_error("Could not configure the isolated CUDA warmup process");
    }
    // Capture the child stdout/stderr so a warmup failure reports the child
    // diagnostics instead of only the exit code.
    SECURITY_ATTRIBUTES pipe_attributes{};
    pipe_attributes.nLength = sizeof(pipe_attributes);
    pipe_attributes.bInheritHandle = TRUE;
    HANDLE child_read = nullptr;
    HANDLE child_write = nullptr;
    const bool capturing = CreatePipe(&child_read, &child_write, &pipe_attributes, 0) != 0
        && SetHandleInformation(child_read, HANDLE_FLAG_INHERIT, 0) != 0;
    if (!capturing) {
        if (child_read != nullptr) CloseHandle(child_read);
        if (child_write != nullptr) CloseHandle(child_write);
        child_read = nullptr;
        child_write = nullptr;
    }
    PROCESS_INFORMATION process{};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (capturing) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = child_write;
        startup.hStdError = child_write;
        startup.hStdInput = nullptr;
    }
    const BOOL created = CreateProcessW(
        executable.data(), command_line.data(), nullptr, nullptr, capturing ? TRUE : FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    if (child_write != nullptr) {
        CloseHandle(child_write);
        child_write = nullptr;
    }
    SetEnvironmentVariableW(kCudaWarmupChildEnvironment, nullptr);
    if (!created) {
        if (child_read != nullptr) CloseHandle(child_read);
        throw std::runtime_error(
            "Could not start the isolated CUDA warmup process; Windows error "
            + std::to_string(create_error));
    }
    const UniqueHandle process_handle(process.hProcess);
    const UniqueHandle thread_handle(process.hThread);
    const UniqueHandle pipe_read(child_read);
    if (WaitForSingleObject(process_handle.get(), INFINITE) != WAIT_OBJECT_0) {
        throw std::runtime_error("Could not wait for the isolated CUDA warmup process");
    }
    const std::string child_output = capturing ? readAllFromHandle(pipe_read.get()) : "";
    DWORD exit_code{};
    if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
        throw std::runtime_error("Could not read the isolated CUDA warmup exit code");
    }
    if (exit_code != 0) {
        std::ostringstream message;
        message << "ONNX CUDA warmup failed in an isolated process with exit code 0x"
                << std::hex << std::uppercase << exit_code;
        const std::string tail = lastChildOutputLines(child_output);
        if (!tail.empty()) message << "; child output: " << tail;
        else if (!capturing) message << "; child output capture unavailable";
        throw std::runtime_error(message.str());
    }
}
#endif

void requestStop(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

#ifdef _WIN32
void applyLiveAnalyticsWindowIcon(std::string_view window_title) {
    const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    if (icon == nullptr) return;
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, window_title.data(), static_cast<int>(window_title.size()), nullptr, 0);
    if (length <= 0) return;
    std::wstring title(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, window_title.data(), static_cast<int>(window_title.size()),
            title.data(), length) <= 0) {
        return;
    }
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, nullptr, title.c_str())) != nullptr) {
        DWORD process_id{};
        GetWindowThreadProcessId(window, &process_id);
        if (process_id != GetCurrentProcessId()) continue;
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        return;
    }
}
#endif

void validateSourceWithoutOpening(const std::string& source) {
    const bool network = isRtspSource(source);
    if (network) {
        validateRtspSource(source);
        return;
    }
    if (!std::filesystem::is_regular_file(source)) {
        throw std::runtime_error("Offline source does not exist: " + source);
    }
}

EnginePipelineConfig enginePipelineConfig(
    const RuntimeConfig& config,
    const ComputeSelection& selection,
    PerformanceTelemetry* telemetry) {
    EnginePipelineConfig pipeline_config{
        selection.backend,
        selection.provider,
        config.ppe_engine,
        config.pose_engine,
        config.ppe_onnx,
        config.pose_onnx,
        config.ppe_labels,
        config.pose_class_count,
        config.pose_keypoint_shape,
        config.allow_nonperson_pose_class,
        config.pose_requires_person,
        config.device,
        config.image_size,
        config.ppe_confidence,
        config.ppe_class_confidences,
        config.ppe_enabled,
        config.pose_confidence,
        config.nms_iou,
        config.max_det,
        {
            config.analytics_mode,
            {
                config.tracker_high_threshold,
                0.10F,
                config.tracker_match_threshold,
                config.tracker_max_age,
                config.tracker_max_tracks,
                config.tracker_frame_rate,
            },
            config.ppe,
            config.fall,
            config.pose_confidence,
            config.nms_iou,
        },
        telemetry,
    };
#ifdef CUAJONE_INTERNAL_DIAGNOSTICS
    // This is deliberately unavailable in production binaries and only applies to offline benchmarks.
    char* serial_hybrid_benchmark{};
    std::size_t serial_hybrid_benchmark_length{};
    if (_dupenv_s(
            &serial_hybrid_benchmark, &serial_hybrid_benchmark_length,
            "CUAJONE_INTERNAL_SERIAL_HYBRID_BENCHMARK") != 0) {
        throw std::runtime_error("Could not read the internal serial hybrid benchmark switch");
    }
    pipeline_config.force_serial_hybrid = !config.benchmark_image.empty()
        && serial_hybrid_benchmark != nullptr && std::string_view(serial_hybrid_benchmark) == "1";
    std::free(serial_hybrid_benchmark);
    char* separate_hybrid_preprocessing{};
    std::size_t separate_hybrid_preprocessing_length{};
    if (_dupenv_s(
            &separate_hybrid_preprocessing, &separate_hybrid_preprocessing_length,
            "CUAJONE_INTERNAL_SEPARATE_HYBRID_PREPROCESSING") != 0) {
        throw std::runtime_error("Could not read the internal separate hybrid preprocessing switch");
    }
    pipeline_config.force_separate_hybrid_preprocessing = !config.benchmark_image.empty()
        && separate_hybrid_preprocessing != nullptr && std::string_view(separate_hybrid_preprocessing) == "1";
    std::free(separate_hybrid_preprocessing);
    char* serial_tensorrt_benchmark{};
    std::size_t serial_tensorrt_benchmark_length{};
    if (_dupenv_s(
            &serial_tensorrt_benchmark, &serial_tensorrt_benchmark_length,
            "CUAJONE_INTERNAL_SERIAL_TENSORRT_BENCHMARK") != 0) {
        throw std::runtime_error("Could not read the internal serial TensorRT benchmark switch");
    }
    pipeline_config.force_serial_tensorrt = !config.benchmark_image.empty()
        && serial_tensorrt_benchmark != nullptr && std::string_view(serial_tensorrt_benchmark) == "1";
    std::free(serial_tensorrt_benchmark);
#endif
    return pipeline_config;
}

std::unique_ptr<NativeEnginePipeline> runBasePreflight(
    const RuntimeConfig& config,
    const ComputeSelection& selection,
    PerformanceTelemetry* telemetry) {
    if (config.benchmark_image.empty()) {
        for (const auto& source : config.sources) validateSourceWithoutOpening(source.source);
        validateWritableOutput(config.output);
    } else if (!std::filesystem::is_regular_file(config.benchmark_image)) {
        throw std::runtime_error("Benchmark image does not exist: " + config.benchmark_image.string());
    }
#ifdef _WIN32
    if (selection.provider == InferenceProvider::OnnxRuntimeCuda && !isCudaWarmupChild()
        && config.benchmark_image.empty()) {
        runIsolatedCudaWarmup();
    }
#endif
    auto pipeline = std::make_unique<NativeEnginePipeline>(enginePipelineConfig(config, selection, telemetry));
#ifdef _WIN32
    if (selection.provider == InferenceProvider::OnnxRuntimeCuda && isCudaWarmupChild()) {
        cv::Mat frame(480, 640, CV_8UC3, cv::Scalar(32, 64, 96));
        const ProcessedFrame warmup = pipeline->processFrame(
            frame, "cuda-preflight", 0, 0, "1970-01-01T00:00:00Z");
        validateCanonicalMetadata(warmup.canonical);
        if (warmup.canonical.frame_width != frame.cols
            || warmup.canonical.frame_height != frame.rows) {
            throw std::runtime_error("ONNX CUDA warmup returned an invalid canonical frame");
        }
    }
#endif
    const auto& summary = pipeline->summary();
    // Contract 1.0.0 allows any camera count: batch-1 ONNX sessions (the only
    // models shipped in the staged bundle) serve N cameras sequentially, one
    // shared engine, per-camera telemetry. A batch optimization profile only
    // makes multi-camera faster (single batched inference), never a gate.
    if (config.sources.size() > summary.maximum_batch_size) {
        std::cout << "Running " << config.sources.size()
                  << " cameras sequentially (managed model maximum batch size "
                  << summary.maximum_batch_size << ")\n";
    }
    std::cout << "OpenCV: " << CV_VERSION << " | provider: " << summary.provider << '\n';
    std::cout << "Maximum inference batch: " << summary.maximum_batch_size << '\n';
    std::cout << "Inference imgsz: " << summary.image_size << 'x' << summary.image_size << '\n';
    if (selection.backend == ComputeBackend::Cuda) {
        std::cout << "CUDA device " << summary.device_index << ": " << summary.device_name
                   << " | SM " << summary.compute_major << '.' << summary.compute_minor
                   << " | devices: " << summary.device_count << '\n';
    }
    if (selection.provider == InferenceProvider::TensorRt) {
        std::cout << "PPE engine: " << config.ppe_engine.string()
                   << " | metadata prefix: " << (summary.ppe_metadata_prefix ? "yes" : "no") << '\n';
        if (summary.pose_loaded) {
            std::cout << "Pose engine: " << config.pose_engine.string()
                       << " | metadata prefix: " << (summary.pose_metadata_prefix ? "yes" : "no") << '\n';
        }
    } else {
        std::cout << "PPE ONNX: " << config.ppe_onnx.string() << '\n';
        if (summary.pose_loaded) std::cout << "Pose ONNX: " << config.pose_onnx.string() << '\n';
    }
    if (config.benchmark_image.empty()) {
        std::cout << "Sources: " << config.sources.size() << " (one shared inference engine)\n";
        for (const auto& source : config.sources) {
            std::cout << "Source: " << redactSource(source.source) << " | label: " << source.label << '\n';
            if (isRtspSource(source.source)) {
                const char* transport = source.rtsp_transport == RtspTransport::Tcp
                    ? "tcp" : source.rtsp_transport == RtspTransport::Udp ? "udp" : "default";
                const char* acceleration = source.video_acceleration == VideoAcceleration::D3d11
                    ? "d3d11" : source.video_acceleration == VideoAcceleration::Cpu ? "cpu" : "auto";
                std::cout << "RTSP transport: " << transport
                          << " | requested video acceleration: " << acceleration
                          << " | read timeout: " << config.capture_read_timeout.count() << " ms\n";
            }
        }
        std::cout << "Output: " << config.output.string() << '\n';
        if (summary.pose_loaded && summary.pose_requires_person) {
            std::cout << "Pose person gate: enabled (pose runs only when PPE detects a person)\n";
        }
    } else {
        std::cout << "Source: benchmark-image\n";
    }
    return pipeline;
}

ModelArtifactAvailability modelArtifactAvailability(const RuntimeConfig& config) {
    return {
        std::filesystem::is_regular_file(config.ppe_engine),
        std::filesystem::is_regular_file(config.pose_engine),
        std::filesystem::is_regular_file(config.ppe_onnx),
        std::filesystem::is_regular_file(config.pose_onnx),
    };
}

void drawPose(cv::Mat& frame, std::span<const Keypoint> keypoints, float threshold) {
    static constexpr std::array<std::array<int, 2>, 16> skeleton{{
        {0, 1}, {0, 2}, {1, 3}, {2, 4}, {5, 6}, {5, 7}, {7, 9}, {6, 8},
        {8, 10}, {5, 11}, {6, 12}, {11, 12}, {11, 13}, {13, 15}, {12, 14}, {14, 16},
    }};
    for (const auto& edge : skeleton) {
        if (edge[0] >= static_cast<int>(keypoints.size()) || edge[1] >= static_cast<int>(keypoints.size())) continue;
        const auto& from = keypoints[edge[0]];
        const auto& to = keypoints[edge[1]];
        if (from.confidence < threshold || to.confidence < threshold) continue;
        cv::line(frame, cv::Point(static_cast<int>(from.x), static_cast<int>(from.y)),
                 cv::Point(static_cast<int>(to.x), static_cast<int>(to.y)),
                 cv::Scalar(255, 140, 0), 2, cv::LINE_AA);
    }
}

void drawPerson(cv::Mat& frame, const CanonicalPerson& person) {
    cv::rectangle(
        frame,
        cv::Point(static_cast<int>(person.box.x1), static_cast<int>(person.box.y1)),
        cv::Point(static_cast<int>(person.box.x2), static_cast<int>(person.box.y2)),
        cv::Scalar(255, 255, 255), 2);
    std::string text = "T" + std::to_string(person.track_id) + " | " + person.ppe_status;
    if (person.fall_active) text += " | POSSIBLE FALL";
    cv::putText(frame, text,
        cv::Point(static_cast<int>(person.box.x1), std::max(25, static_cast<int>(person.box.y1) - 10)),
        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
    if (!person.ppe) return;
    int line_y = static_cast<int>(person.box.y1) + 18;
    for (const auto& item : person.ppe->items) {
        if (!item.enabled) continue;
        const std::string state = !person.ppe->evaluated ? "..." : std::string(ppeWearStateName(item.wear_state));
        const cv::Scalar color = !person.ppe->evaluated || item.wear_state == PpeWearState::NotVerifiable
            ? cv::Scalar(0, 220, 255) : item.wear_state == PpeWearState::PresentCorrectly
            ? cv::Scalar(0, 220, 0) : cv::Scalar(0, 0, 255);
        cv::putText(frame,
            std::string(ppeItemLabel(item.item)) + ": " + state,
            cv::Point(static_cast<int>(person.box.x1) + 4, line_y),
            cv::FONT_HERSHEY_SIMPLEX, 0.42, color, 1, cv::LINE_AA);
        line_y += 17;
    }
}

std::string observedAtUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &time);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
    return output.str();
}

void drawAssociatedItem(cv::Mat& frame, const std::optional<Detection>& item, const std::string& label) {
    if (!item) return;
    cv::rectangle(frame,
        cv::Point(static_cast<int>(item->box.x1), static_cast<int>(item->box.y1)),
        cv::Point(static_cast<int>(item->box.x2), static_cast<int>(item->box.y2)),
        cv::Scalar(0, 220, 255), 2);
    cv::putText(frame, label, cv::Point(static_cast<int>(item->box.x1), std::max(20, static_cast<int>(item->box.y1) - 6)),
        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 220, 255), 2, cv::LINE_AA);
}

[[maybe_unused]] void drawPerformanceOverlay(
    cv::Mat& frame,
    const OverlayMetrics& metrics,
    bool source_connected = true) {
    constexpr int kPadding = 10;
    const int line_height = 18;
    const int line_count = 14;
    const int width = std::min(frame.cols, std::min(390, std::max(280, frame.cols / 3)));
    const int height = kPadding * 2 + line_count * line_height;
    const cv::Rect panel(0, 0, width, std::min(height, frame.rows));
    cv::Mat panel_pixels = frame(panel);
    cv::Mat black(panel_pixels.size(), panel_pixels.type(), cv::Scalar(0, 0, 0));
    cv::addWeighted(black, 0.62, panel_pixels, 0.38, 0.0, panel_pixels);
    const std::vector<std::string> lines{
        "Fotogramas por segundo: " + std::format("{:.2f}", metrics.displayed_fps),
        "Codec de video: " + (metrics.video_codec.empty() ? "No disponible" : metrics.video_codec),
        "Resolucion de video: " + std::to_string(metrics.frame_width) + "x" + std::to_string(metrics.frame_height),
        "Backend de captura: " + (metrics.capture_backend.empty() ? "No disponible" : metrics.capture_backend),
        "Decodificacion de video: " + (metrics.video_acceleration.empty() ? "No disponible" : metrics.video_acceleration),
        std::string("Estado de fuente: ") + (source_connected ? "Conectada" : "Reconectando"),
        std::string("Disponibilidad de imagen: ") + (source_connected ? "Disponible" : "Cuadro retenido"),
        "Fotogramas por segundo (recibidos): " + std::format("{:.2f}", metrics.received_fps),
        "Backend de inferencia: " + (metrics.backend.empty() ? "n/a" : metrics.backend),
        "GPU Name: " + (metrics.device_name.empty() ? "n/a" : metrics.device_name),
        "Latencia total p50: " + std::format("{:.1f} ms", metrics.pipeline_p50_ms),
        "Inferencia EPP p50: " + std::format("{:.1f} ms", metrics.ppe_inference_p50_ms),
        "Inferencia pose p50: " + std::format("{:.1f} ms", metrics.pose_inference_p50_ms),
        "Frames omitidos: " + std::to_string(metrics.dropped_frames),
    };
    for (std::size_t index = 0; index < lines.size(); ++index) {
        cv::putText(frame, lines[index], cv::Point(kPadding, kPadding + static_cast<int>(index + 1) * line_height - 4),
            cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(235, 235, 235), 1, cv::LINE_AA);
    }
}

[[maybe_unused]] void drawReconnectBanner(cv::Mat& frame) {
    if (frame.empty()) return;
    constexpr int kHeight = 58;
    const int top = std::max(0, frame.rows - kHeight);
    const cv::Rect banner(0, top, frame.cols, frame.rows - top);
    cv::Mat pixels = frame(banner);
    cv::Mat background(pixels.size(), pixels.type(), cv::Scalar(10, 10, 45));
    cv::addWeighted(background, 0.78, pixels, 0.22, 0.0, pixels);
    cv::putText(frame, "Reconectando a la camara...", cv::Point(16, top + 25),
        cv::FONT_HERSHEY_SIMPLEX, 0.62, cv::Scalar(245, 245, 255), 2, cv::LINE_AA);
    cv::putText(frame, "La ventana sigue activa; presiona Esc o Q para detener.", cv::Point(16, top + 48),
        cv::FONT_HERSHEY_SIMPLEX, 0.44, cv::Scalar(210, 210, 230), 1, cv::LINE_AA);
}

struct GridLayout {
    std::size_t rows{};
    std::size_t cols{};
};

// Pure helper: balanced grid with cols=ceil(sqrt(n)), rows=ceil(n/cols).
// 1->1x1, 2->1x2, 3-4->2x2, 5-6->2x3, ... Surplus cells stay black.
GridLayout computeGridLayout(std::size_t source_count) {
    if (source_count == 0) return {0, 0};
    const auto cols = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(source_count))));
    const auto rows = (source_count + cols - 1) / cols;
    return {rows, cols};
}

// Fits a frame into a fixed-size cell preserving aspect ratio (resize +
// centered black letterbox). Empty input yields a black cell so one failed
// decode never blanks the grid.
[[maybe_unused]] cv::Mat fitFrameToCell(const cv::Mat& frame, int cell_width, int cell_height) {
    if (frame.empty() || cell_width <= 0 || cell_height <= 0) {
        return cv::Mat(std::max(cell_height, 1), std::max(cell_width, 1), CV_8UC3,
            cv::Scalar(0, 0, 0));
    }
    const double scale = std::min(
        static_cast<double>(cell_width) / static_cast<double>(frame.cols),
        static_cast<double>(cell_height) / static_cast<double>(frame.rows));
    const int scaled_width = std::max(1, static_cast<int>(std::round(frame.cols * scale)));
    const int scaled_height = std::max(1, static_cast<int>(std::round(frame.rows * scale)));
    cv::Mat scaled;
    cv::resize(frame, scaled, cv::Size(scaled_width, scaled_height), 0.0, 0.0, cv::INTER_LINEAR);
    const int top = (cell_height - scaled_height) / 2;
    const int bottom = cell_height - scaled_height - top;
    const int left = (cell_width - scaled_width) / 2;
    const int right = cell_width - scaled_width - left;
    cv::Mat cell;
    cv::copyMakeBorder(scaled, cell, top, bottom, left, right, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return cell;
}

// Composes equally-sized tiles (row-major) into a single canvas; missing
// cells are black.
[[maybe_unused]] cv::Mat composeGridCanvas(
    const std::vector<cv::Mat>& tiles, const GridLayout& layout, int cell_width, int cell_height) {
    std::vector<cv::Mat> rows;
    rows.reserve(layout.rows);
    std::size_t index = 0;
    const cv::Mat black_cell(cell_height, cell_width, CV_8UC3, cv::Scalar(0, 0, 0));
    for (std::size_t row = 0; row < layout.rows; ++row) {
        std::vector<cv::Mat> row_cells;
        row_cells.reserve(layout.cols);
        for (std::size_t col = 0; col < layout.cols; ++col) {
            row_cells.push_back(index < tiles.size() ? tiles[index] : black_cell);
            ++index;
        }
        cv::Mat row_image;
        cv::hconcat(row_cells, row_image);
        rows.push_back(std::move(row_image));
    }
    cv::Mat canvas;
    cv::vconcat(rows, canvas);
    return canvas;
}

// Single-window resizable mosaic.
// Gesture (documented on the canvas help strip as well):
// - Hover a tile to reveal its camera name (auto-hides after 2 s without mouse).
// - Drag a divider border between tiles to resize: the two adjacent
//   column/row weights are rebalanced from the pointer position, then the
//   layout is recomputed. Dividers have an 8 px grab zone.
// - Keyboard on the focused tile: '+'/'=' enlarge, '-'/'_' shrink,
//   '['/']' move focus, '0' resets all weights to 1.0.
struct MosaicUiState {
    GridLayout layout{};
    std::vector<double> col_weights;
    std::vector<double> row_weights;
    std::vector<cv::Rect> tile_rects;
    int canvas_width{};
    int canvas_height{};
    int hover_tile{-1};
    Clock::time_point hover_time{Clock::time_point::min()};
    int focused_tile{};
    bool dragging{};
    int drag_col{-1};
    int drag_row{-1};
};

constexpr int kDividerGrabPixels = 8;
constexpr double kMinTileWeight = 0.20;
constexpr double kFocusStepFactor = 1.10;
constexpr auto kHoverLabelTimeout = std::chrono::seconds(2);

void initMosaicWeights(MosaicUiState& state) {
    state.col_weights.assign(state.layout.cols, 1.0);
    state.row_weights.assign(state.layout.rows, 1.0);
}

// Splits the fixed canvas budget proportionally to the weights. The total
// canvas size never changes, only the share of each column/row.
std::vector<cv::Rect> weightedTileRects(
    const GridLayout& layout,
    const std::vector<double>& col_weights,
    const std::vector<double>& row_weights,
    int canvas_width,
    int canvas_height) {
    std::vector<cv::Rect> rects;
    if (layout.cols == 0 || layout.rows == 0 || canvas_width <= 0 || canvas_height <= 0) return rects;
    double col_total = 0.0;
    double row_total = 0.0;
    for (double w : col_weights) col_total += std::max(w, 1e-6);
    for (double w : row_weights) row_total += std::max(w, 1e-6);
    std::vector<int> col_widths(layout.cols, 0);
    std::vector<int> row_heights(layout.rows, 0);
    int used_width = 0;
    int used_height = 0;
    for (std::size_t col = 0; col < layout.cols; ++col) {
        const double share = std::max(col_weights[col], 1e-6) / col_total;
        col_widths[col] = (col + 1 == layout.cols)
            ? canvas_width - used_width
            : std::max(1, static_cast<int>(std::round(canvas_width * share)));
        used_width += col_widths[col];
    }
    for (std::size_t row = 0; row < layout.rows; ++row) {
        const double share = std::max(row_weights[row], 1e-6) / row_total;
        row_heights[row] = (row + 1 == layout.rows)
            ? canvas_height - used_height
            : std::max(1, static_cast<int>(std::round(canvas_height * share)));
        used_height += row_heights[row];
    }
    rects.reserve(layout.rows * layout.cols);
    int y = 0;
    for (std::size_t row = 0; row < layout.rows; ++row) {
        int x = 0;
        for (std::size_t col = 0; col < layout.cols; ++col) {
            rects.emplace_back(x, y, col_widths[col], row_heights[row]);
            x += col_widths[col];
        }
        y += row_heights[row];
    }
    return rects;
}

int mosaicTileAt(const MosaicUiState& state, int x, int y) {
    for (std::size_t index = 0; index < state.tile_rects.size(); ++index) {
        if (state.tile_rects[index].contains(cv::Point(x, y))) return static_cast<int>(index);
    }
    return -1;
}

void mosaicClampWeights(MosaicUiState& state) {
    for (double& w : state.col_weights) w = std::clamp(w, kMinTileWeight, 10.0);
    for (double& w : state.row_weights) w = std::clamp(w, kMinTileWeight, 10.0);
}

void mosaicRebalanceColumns(MosaicUiState& state, int divider_col, int mouse_x) {
    if (divider_col < 0 || static_cast<std::size_t>(divider_col + 1) >= state.col_weights.size()) return;
    if (state.tile_rects.empty() || state.canvas_width <= 0) return;
    const int left_edge = state.tile_rects[divider_col].x;
    const int right_edge = state.tile_rects[divider_col + 1].x + state.tile_rects[divider_col + 1].width;
    const int span = std::max(1, right_edge - left_edge);
    // The divider spans every row, so the full two-column span is the unit.
    double fraction = static_cast<double>(mouse_x - left_edge) / static_cast<double>(span);
    fraction = std::clamp(fraction, 0.10, 0.90);
    const double pair_total = state.col_weights[divider_col] + state.col_weights[divider_col + 1];
    state.col_weights[divider_col] = std::max(kMinTileWeight, pair_total * fraction);
    state.col_weights[divider_col + 1] = std::max(kMinTileWeight, pair_total * (1.0 - fraction));
}

void mosaicRebalanceRows(MosaicUiState& state, int divider_row, int mouse_y) {
    if (divider_row < 0 || static_cast<std::size_t>(divider_row + 1) >= state.row_weights.size()) return;
    if (state.tile_rects.empty() || state.canvas_height <= 0) return;
    const std::size_t cols = state.layout.cols;
    const int top_edge = state.tile_rects[static_cast<std::size_t>(divider_row) * cols].y;
    const int bottom_edge =
        state.tile_rects[static_cast<std::size_t>(divider_row + 1) * cols].y
        + state.tile_rects[static_cast<std::size_t>(divider_row + 1) * cols].height;
    const int span = std::max(1, bottom_edge - top_edge);
    double fraction = static_cast<double>(mouse_y - top_edge) / static_cast<double>(span);
    fraction = std::clamp(fraction, 0.10, 0.90);
    const double pair_total = state.row_weights[divider_row] + state.row_weights[divider_row + 1];
    state.row_weights[divider_row] = std::max(kMinTileWeight, pair_total * fraction);
    state.row_weights[divider_row + 1] = std::max(kMinTileWeight, pair_total * (1.0 - fraction));
}

void onMosaicMouse(int event, int x, int y, int flags, void* userdata) {
    auto* state = static_cast<MosaicUiState*>(userdata);
    if (state == nullptr || state->tile_rects.empty()) return;
    const auto now = Clock::now();
    if (event == cv::EVENT_MOUSEMOVE || event == cv::EVENT_LBUTTONDOWN) {
        const int hit = mosaicTileAt(*state, x, y);
        if (hit >= 0) {
            state->hover_tile = hit;
            state->hover_time = now;
        }
    }
    if (event == cv::EVENT_LBUTTONDOWN) {
        // Prefer the vertical divider when both are near (corners).
        for (std::size_t col = 0; col + 1 < state->layout.cols; ++col) {
            const int boundary = state->tile_rects[col].x + state->tile_rects[col].width;
            if (std::abs(x - boundary) <= kDividerGrabPixels) {
                state->dragging = true;
                state->drag_col = static_cast<int>(col);
                state->drag_row = -1;
                return;
            }
        }
        for (std::size_t row = 0; row + 1 < state->layout.rows; ++row) {
            const int boundary =
                state->tile_rects[row * state->layout.cols].y
                + state->tile_rects[row * state->layout.cols].height;
            if (std::abs(y - boundary) <= kDividerGrabPixels) {
                state->dragging = true;
                state->drag_row = static_cast<int>(row);
                state->drag_col = -1;
                return;
            }
        }
        const int hit = mosaicTileAt(*state, x, y);
        if (hit >= 0) state->focused_tile = hit;
        state->dragging = false;
    } else if (event == cv::EVENT_MOUSEMOVE && state->dragging
        && (flags & cv::EVENT_FLAG_LBUTTON) != 0) {
        if (state->drag_col >= 0) mosaicRebalanceColumns(*state, state->drag_col, x);
        else if (state->drag_row >= 0) mosaicRebalanceRows(*state, state->drag_row, y);
    } else if (event == cv::EVENT_LBUTTONUP) {
        state->dragging = false;
        state->drag_col = -1;
        state->drag_row = -1;
    }
}

// Pastes the clean (unannotated) frame into the tile pixel-perfect 1:1
// (never upscales; only downscales when the native frame does not fit the
// tile). The frame stays centered over the pre-painted black tile, so any
// leftover area shows black bars/surround. NEVER stretches to fill the tile.
// Returns the scale/offset mapping source pixels to canvas pixels so every
// overlay can be drawn afterwards at canvas resolution.
struct LetterboxMap {
    double scale{1.0};
    int origin_x{};
    int origin_y{};
    int content_width{};
    int content_height{};
};

LetterboxMap pasteLetterboxed(cv::Mat& canvas, const cv::Rect& tile, const cv::Mat& frame) {
    LetterboxMap mapping;
    if (tile.width <= 0 || tile.height <= 0) return mapping;
    cv::Mat destination = canvas(tile & cv::Rect(0, 0, canvas.cols, canvas.rows));
    destination.setTo(cv::Scalar(0, 0, 0));
    if (frame.empty()) return mapping;
    // Pixel-perfect 1:1: uniform scale capped at 1.0 + centered black surround.
    const double scale = std::min({1.0,
        static_cast<double>(tile.width) / static_cast<double>(frame.cols),
        static_cast<double>(tile.height) / static_cast<double>(frame.rows)});
    const int scaled_width = std::max(1, static_cast<int>(std::round(frame.cols * scale)));
    const int scaled_height = std::max(1, static_cast<int>(std::round(frame.rows * scale)));
    cv::Mat content;
    if (scale >= 1.0) {
        content = frame;
    } else {
        cv::resize(frame, content, cv::Size(scaled_width, scaled_height), 0.0, 0.0, cv::INTER_LINEAR);
    }
    const int offset_x = (tile.width - scaled_width) / 2;
    const int offset_y = (tile.height - scaled_height) / 2;
    cv::Mat roi = destination(cv::Rect(offset_x, offset_y, scaled_width, scaled_height));
    content.copyTo(roi);
    mapping.scale = scale;
    mapping.origin_x = tile.x + offset_x;
    mapping.origin_y = tile.y + offset_y;
    mapping.content_width = scaled_width;
    mapping.content_height = scaled_height;
    return mapping;
}

cv::Point mapToCanvas(const LetterboxMap& mapping, float x, float y) {
    return {
        mapping.origin_x + static_cast<int>(std::round(x * mapping.scale)),
        mapping.origin_y + static_cast<int>(std::round(y * mapping.scale)),
    };
}

// Canvas-resolution detection rendering: boxes, skeleton and text are drawn
// AFTER the pixel-perfect paste, so they stay sharp at any tile size.
void drawDetectionsOnCanvas(
    cv::Mat& canvas,
    const LetterboxMap& mapping,
    const std::vector<CanonicalPerson>& people,
    const std::map<int, PpeAssociation>& associations,
    AnalyticsMode analytics_mode,
    float keypoint_threshold) {
    static constexpr std::array<std::array<int, 2>, 16> kSkeleton{{
        {0, 1}, {0, 2}, {1, 3}, {2, 4}, {5, 6}, {5, 7}, {7, 9}, {6, 8},
        {8, 10}, {5, 11}, {6, 12}, {11, 12}, {11, 13}, {13, 15}, {12, 14}, {14, 16},
    }};
    for (const auto& person : people) {
        if (analytics_mode == AnalyticsMode::PpeFall && !person.keypoints.empty()) {
            for (const auto& edge : kSkeleton) {
                if (edge[0] >= static_cast<int>(person.keypoints.size())
                    || edge[1] >= static_cast<int>(person.keypoints.size())) continue;
                const auto& from = person.keypoints[edge[0]];
                const auto& to = person.keypoints[edge[1]];
                if (from.confidence < keypoint_threshold || to.confidence < keypoint_threshold) continue;
                cv::line(canvas, mapToCanvas(mapping, from.x, from.y),
                    mapToCanvas(mapping, to.x, to.y), cv::Scalar(255, 140, 0), 2, cv::LINE_AA);
            }
        }
        cv::rectangle(canvas, mapToCanvas(mapping, person.box.x1, person.box.y1),
            mapToCanvas(mapping, person.box.x2, person.box.y2),
            cv::Scalar(255, 255, 255), 2);
        std::string text = "T" + std::to_string(person.track_id) + " | " + person.ppe_status;
        if (person.fall_active) text += " | POSSIBLE FALL";
        const cv::Point anchor = mapToCanvas(mapping, person.box.x1, person.box.y1);
        cv::putText(canvas, text, {anchor.x, std::max(mapping.origin_y + 16, anchor.y - 8)},
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
        if (!person.ppe) continue;
        int line_y = anchor.y + 16;
        for (const auto& item : person.ppe->items) {
            if (!item.enabled) continue;
            const std::string state =
                !person.ppe->evaluated ? "..." : std::string(ppeWearStateName(item.wear_state));
            const cv::Scalar color = !person.ppe->evaluated || item.wear_state == PpeWearState::NotVerifiable
                ? cv::Scalar(0, 220, 255) : item.wear_state == PpeWearState::PresentCorrectly
                ? cv::Scalar(0, 220, 0) : cv::Scalar(0, 0, 255);
            cv::putText(canvas, std::string(ppeItemLabel(item.item)) + ": " + state,
                {anchor.x + 4, line_y}, cv::FONT_HERSHEY_SIMPLEX, 0.42, color, 1, cv::LINE_AA);
            line_y += 17;
        }
        const auto association = associations.find(person.track_id);
        if (association == associations.end()) continue;
        for (const PpeItem item : requiredPpeItems()) {
            const auto detection = association->second.detection(item);
            if (!detection) continue;
            cv::rectangle(canvas,
                mapToCanvas(mapping, detection->box.x1, detection->box.y1),
                mapToCanvas(mapping, detection->box.x2, detection->box.y2),
                cv::Scalar(0, 220, 255), 2);
            const cv::Point item_anchor = mapToCanvas(mapping, detection->box.x1, detection->box.y1);
            cv::putText(canvas, std::string(ppeItemLabel(item)),
                {item_anchor.x, std::max(mapping.origin_y + 14, item_anchor.y - 6)},
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 220, 255), 2, cv::LINE_AA);
        }
    }
}

// Performance panel drawn at canvas resolution inside the tile, never
// pre-scaled on the small frame.
void drawPerformancePanelOnCanvas(
    cv::Mat& canvas, const cv::Rect& tile, const OverlayMetrics& metrics, bool connected) {
    constexpr int kPadding = 8;
    constexpr int kLineHeight = 16;
    constexpr int kLineCount = 14;
    const int width = std::min(tile.width, std::min(390, std::max(280, tile.width / 2)));
    const int height = std::min(tile.height, kPadding * 2 + kLineCount * kLineHeight);
    if (width <= 10 || height <= 10) return;
    const cv::Rect panel(tile.x, tile.y, width, height);
    const cv::Rect clipped = panel & cv::Rect(0, 0, canvas.cols, canvas.rows);
    if (clipped.empty()) return;
    cv::Mat pixels = canvas(clipped);
    cv::Mat black(pixels.size(), pixels.type(), cv::Scalar(0, 0, 0));
    cv::addWeighted(black, 0.62, pixels, 0.38, 0.0, pixels);
    const std::vector<std::string> lines{
        "Fotogramas por segundo: " + std::format("{:.2f}", metrics.displayed_fps),
        "Codec de video: " + (metrics.video_codec.empty() ? "No disponible" : metrics.video_codec),
        "Resolucion de video: " + std::to_string(metrics.frame_width) + "x" + std::to_string(metrics.frame_height),
        "Backend de captura: " + (metrics.capture_backend.empty() ? "No disponible" : metrics.capture_backend),
        "Decodificacion de video: " + (metrics.video_acceleration.empty() ? "No disponible" : metrics.video_acceleration),
        std::string("Estado de fuente: ") + (connected ? "Conectada" : "Reconectando"),
        std::string("Disponibilidad de imagen: ") + (connected ? "Disponible" : "Cuadro retenido"),
        "Fotogramas por segundo (recibidos): " + std::format("{:.2f}", metrics.received_fps),
        "Backend de inferencia: " + (metrics.backend.empty() ? "n/a" : metrics.backend),
        "GPU Name: " + (metrics.device_name.empty() ? "n/a" : metrics.device_name),
        "Latencia total p50: " + std::format("{:.1f} ms", metrics.pipeline_p50_ms),
        "Inferencia EPP p50: " + std::format("{:.1f} ms", metrics.ppe_inference_p50_ms),
        "Inferencia pose p50: " + std::format("{:.1f} ms", metrics.pose_inference_p50_ms),
        "Frames omitidos: " + std::to_string(metrics.dropped_frames),
    };
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int text_y = tile.y + kPadding + static_cast<int>(index + 1) * kLineHeight - 4;
        if (text_y > tile.y + tile.height - 2) break;
        cv::putText(canvas, lines[index], {tile.x + kPadding, text_y},
            cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(235, 235, 235), 1, cv::LINE_AA);
    }
}

void drawReconnectBannerOnCanvas(cv::Mat& canvas, const cv::Rect& tile) {
    if (canvas.empty() || tile.width <= 0 || tile.height <= 0) return;
    constexpr int kHeight = 52;
    const int banner_height = std::min(kHeight, tile.height / 3);
    const cv::Rect banner(tile.x, tile.y + tile.height - banner_height, tile.width, banner_height);
    const cv::Rect clipped = banner & cv::Rect(0, 0, canvas.cols, canvas.rows);
    if (clipped.empty()) return;
    cv::Mat pixels = canvas(clipped);
    cv::Mat background(pixels.size(), pixels.type(), cv::Scalar(10, 10, 45));
    cv::addWeighted(background, 0.78, pixels, 0.22, 0.0, pixels);
    cv::putText(canvas, "Reconectando a la camara...", {tile.x + 12, banner.y + 20},
        cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(245, 245, 255), 2, cv::LINE_AA);
    if (banner_height >= 44) {
        cv::putText(canvas, "La ventana sigue activa; presiona Esc o Q para detener.",
            {tile.x + 12, banner.y + 40}, cv::FONT_HERSHEY_SIMPLEX, 0.40,
            cv::Scalar(210, 210, 230), 1, cv::LINE_AA);
    }
}

[[maybe_unused]] bool liveWindowStopRequested(std::string_view window_title) {
    const int key = cv::waitKey(1) & 0xFF;
    if (key == 'q' || key == 27) return true;
    // Intentional semantic change: there is a single composed window, so
    // closing it stops every stream (previously any of N windows stopped).
    return cv::getWindowProperty(std::string(window_title), cv::WND_PROP_VISIBLE) < 1.0;
}

// Extended poll: keeps the single-window stop semantics and applies the
// resizable-mosaic keys on the focused tile.
bool pollLiveWindow(MosaicUiState& state, std::string_view window_title) {
    const int key = cv::waitKey(1) & 0xFF;
    if (key == 'q' || key == 27) return true;
    if (key == '+' || key == '=') {
        if (!state.col_weights.empty() && !state.row_weights.empty()
            && !state.tile_rects.empty()) {
            const std::size_t focus = static_cast<std::size_t>(
                std::clamp(state.focused_tile, 0,
                    static_cast<int>(state.tile_rects.size()) - 1));
            state.col_weights[focus % state.layout.cols] *= kFocusStepFactor;
            state.row_weights[focus / state.layout.cols] *= kFocusStepFactor;
            mosaicClampWeights(state);
        }
    } else if (key == '-' || key == '_') {
        if (!state.col_weights.empty() && !state.row_weights.empty()
            && !state.tile_rects.empty()) {
            const std::size_t focus = static_cast<std::size_t>(
                std::clamp(state.focused_tile, 0,
                    static_cast<int>(state.tile_rects.size()) - 1));
            state.col_weights[focus % state.layout.cols] /= kFocusStepFactor;
            state.row_weights[focus / state.layout.cols] /= kFocusStepFactor;
            mosaicClampWeights(state);
        }
    } else if (key == ']') {
        if (!state.tile_rects.empty()) {
            state.focused_tile =
                (state.focused_tile + 1) % static_cast<int>(state.tile_rects.size());
        }
    } else if (key == '[') {
        if (!state.tile_rects.empty()) {
            state.focused_tile =
                (state.focused_tile - 1 + static_cast<int>(state.tile_rects.size()))
                % static_cast<int>(state.tile_rects.size());
        }
    } else if (key == '0') {
        initMosaicWeights(state);
    }
    return cv::getWindowProperty(std::string(window_title), cv::WND_PROP_VISIBLE) < 1.0;
}

int monitor(
    const RuntimeConfig& config,
    NativeEnginePipeline& pipeline,
    PerformanceTelemetry* telemetry) {
    std::optional<EvidenceWriter> evidence;
    std::optional<EvidenceWriterV3> evidence_v3;
    std::unique_ptr<EvidenceWriterQueue> evidence_queue;
    if (config.evidence_writer_queue_capacity == 0) {
        evidence.emplace(config.output);
        evidence_v3.emplace(config.output);
        if (telemetry != nullptr) telemetry->setEvidenceQueueTelemetry({false});
    } else {
        evidence_queue = std::make_unique<EvidenceWriterQueue>(
            config.output, config.evidence_writer_queue_capacity);
    }
    struct SourceState {
        const RuntimeSource* config{};
        std::unique_ptr<LatestFrameCapture> capture;
        std::uint64_t sequence{};
        Clock::time_point next_inference{Clock::time_point::min()};
        std::optional<std::string> last_capture_error;
        // Clean (unannotated) frame for display composition. Detection and
        // performance overlays are rendered later at canvas resolution so
        // they never suffer a small-frame upscale. The annotated frame used
        // for evidence keeps the original small-frame drawing path intact.
        cv::Mat last_displayed_frame;
        std::vector<CanonicalPerson> last_people;
        std::map<int, PpeAssociation> last_associations;
        float last_keypoint_threshold{0.35F};
        AnalyticsMode last_analytics_mode{AnalyticsMode::PpeFall};
        OverlayMetrics last_metrics{};
        bool has_metrics{};
        bool last_connected{true};
        bool first_inference_logged{};
        bool display_dirty{};
    };
    std::vector<SourceState> sources;
    sources.reserve(config.sources.size());
    for (const auto& source : config.sources) {
        SourceState state;
        state.config = &source;
        state.capture = std::make_unique<LatestFrameCapture>(
            source.source,
            std::chrono::duration<double>(config.reconnect_delay_seconds),
            std::chrono::duration<double>(config.maximum_reconnect_delay_seconds),
            config.capture_open_timeout,
            config.capture_read_timeout,
            source.rtsp_transport,
            source.video_acceleration,
            telemetry);
        state.capture->start();
        sources.push_back(std::move(state));
    }
    const GridLayout grid_layout = computeGridLayout(sources.size());
    // Cap the composed canvas width so the single window stays manageable.
    constexpr int kMaxCanvasWidth = 1920;
    // Fixed 16:9 display canvas. The Win32 HighGUI backend stretches a
    // WINDOW_NORMAL image to the window client area, so a raw grid canvas
    // (e.g. 1920x540 for a 1x2 mosaic) is stretched vertically ~2x when the
    // ~16:9 window is maximized. Centering the grid on a 1920x1080 canvas
    // makes maximized display ~1:1: HighGUI borders would appear instead of
    // deformation on backends honoring KEEPRATIO, and Win32 shows black
    // letterbox bars baked into the frame. Grid layouts never exceed these
    // bounds (rows <= cols, so height <= 1080), so centering offsets are >= 0.
    constexpr int kDisplayWidth = 1920;
    constexpr int kDisplayHeight = 1080;
    const int base_cell_width = grid_layout.cols == 0
        ? kMaxCanvasWidth
        : std::max(160, kMaxCanvasWidth / static_cast<int>(grid_layout.cols));
    const int base_cell_height = base_cell_width * 9 / 16;
    const int canvas_width = static_cast<int>(grid_layout.cols) * base_cell_width;
    const int canvas_height = static_cast<int>(grid_layout.rows) * base_cell_height;
    MosaicUiState mosaic{};
    mosaic.layout = grid_layout;
    mosaic.canvas_width = canvas_width;
    mosaic.canvas_height = canvas_height;
    initMosaicWeights(mosaic);
    std::vector<double> last_composed_cols;
    std::vector<double> last_composed_rows;
    bool grid_shown{};
    const std::chrono::duration<double> telemetry_interval(config.telemetry_interval_seconds);
    const bool periodic_telemetry = telemetry != nullptr && telemetry_interval.count() > 0.0;
    auto next_snapshot = Clock::now() + telemetry_interval;
    if (config.show_window) {
        // Keep the window aspect: KEEPRATIO is a no-op value-wise
        // (WINDOW_KEEPRATIO == 0) but documents intent; the Win32 HighGUI
        // backend stretches WINDOW_NORMAL content to the client area, so the
        // real guarantee comes from composing a fixed 16:9 display canvas
        // below. The explicit aspect-ratio property helps backends that honor
        // it (e.g. Qt) without affecting Win32.
        cv::namedWindow(kLiveAnalyticsWindowTitle, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
        cv::setWindowProperty(
            kLiveAnalyticsWindowTitle, cv::WND_PROP_ASPECT_RATIO, cv::WINDOW_KEEPRATIO);
        cv::resizeWindow(kLiveAnalyticsWindowTitle, kDisplayWidth, kDisplayHeight);
#ifdef _WIN32
        applyLiveAnalyticsWindowIcon(kLiveAnalyticsWindowTitle);
#endif
        cv::setMouseCallback(kLiveAnalyticsWindowTitle, onMosaicMouse, &mosaic);
    }

    while (!stop_requested.load(std::memory_order_relaxed)) {
        if (periodic_telemetry && Clock::now() >= next_snapshot) {
            // Non-resetting snapshot: jsonReport only reads counters and rolling
            // samples, so periodic emission never disturbs the exit-only report.
            std::cerr << "Telemetry snapshot: " << telemetry->jsonReport() << '\n';
            next_snapshot = Clock::now() + telemetry_interval;
        }
        bool received_any{};
        bool any_running{};
        std::vector<SourceState*> ready_states;
        std::vector<EngineFrameInput> batch_inputs;
        ready_states.reserve(sources.size());
        batch_inputs.reserve(sources.size());
        for (auto& source : sources) {
            if (!source.capture->ended()) any_running = true;
            cv::Mat frame;
            std::uint64_t latest_sequence = source.sequence;
            Clock::time_point published_at;
            if (!source.capture->waitForLatest(
                    source.sequence, frame, latest_sequence, published_at,
                    std::chrono::milliseconds(1))) {
                const auto error = source.capture->lastError();
                if (error && error != source.last_capture_error) {
                    std::cerr << "Capture [" << source.config->label << "]: " << *error << '\n';
                    source.last_capture_error = error;
                }
                if (config.show_window && error) {
                    // Keep the last clean frame; the reconnect banner and the
                    // performance panel are drawn later at canvas resolution.
                    if (source.last_displayed_frame.empty()) {
                        source.last_displayed_frame =
                            cv::Mat(720, 1280, CV_8UC3, cv::Scalar(24, 24, 24));
                    }
                    if (config.performance_report && telemetry != nullptr) {
                        source.last_metrics = telemetry->overlayMetrics(
                            source.last_displayed_frame.cols, source.last_displayed_frame.rows);
                        source.has_metrics = true;
                    }
                    source.last_connected = false;
                    source.display_dirty = true;
                }
                continue;
            }
            received_any = true;
            source.last_capture_error.reset();
            if (telemetry != nullptr) telemetry->recordLatestSlotSequence(source.sequence, latest_sequence);
            source.sequence = latest_sequence;
            const auto now = Clock::now();
            if (telemetry != nullptr) telemetry->addSample(PerformanceStage::FrameAge, now - published_at);
            if (config.target_fps > 0.0 && now < source.next_inference) {
                if (telemetry != nullptr) telemetry->skippedForTargetFps();
                continue;
            }
            if (config.target_fps > 0.0) {
                source.next_inference = now + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(1.0 / config.target_fps));
            }
            const auto monotonic_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            ready_states.push_back(&source);
            batch_inputs.push_back({
                frame, source.config->label, source.sequence, monotonic_ms, observedAtUtc(),
            });
        }
        if (!batch_inputs.empty()) {
            // Sequential multi-camera mode: split the ready frames into
            // profile-sized micro-batches (batch-1 models yield one chunk per
            // camera) so processBatch never exceeds the managed model maximum
            // batch size. Chunk order matches ready_states, keeping per-camera
            // telemetry and the single composed grid aligned.
            const std::size_t chunk_size = std::max<std::size_t>(
                1, pipeline.summary().maximum_batch_size);
            std::vector<ProcessedFrame> processed_batch;
            processed_batch.reserve(batch_inputs.size());
            for (std::size_t offset = 0; offset < batch_inputs.size(); offset += chunk_size) {
                const std::size_t count = std::min(chunk_size, batch_inputs.size() - offset);
                const std::span<const EngineFrameInput> chunk(batch_inputs.data() + offset, count);
                auto chunk_result = pipeline.processBatch(chunk);
                processed_batch.insert(processed_batch.end(),
                    std::make_move_iterator(chunk_result.begin()),
                    std::make_move_iterator(chunk_result.end()));
            }
            if (processed_batch.size() != batch_inputs.size()) {
                throw std::runtime_error("Inference pipeline returned an incomplete micro-batch");
            }
            for (std::size_t batch_index = 0; batch_index < processed_batch.size(); ++batch_index) {
                SourceState& source = *ready_states[batch_index];
                cv::Mat frame = batch_inputs[batch_index].frame;
                const ProcessedFrame& processed = processed_batch[batch_index];
                if (telemetry != nullptr) telemetry->processedFrame();
                if (!source.first_inference_logged) {
                    std::cout << "Inference [" << source.config->label << "]: first frame processed"
                              << " | micro-batch: " << processed_batch.size()
                              << " | people: " << processed.canonical.people.size() << '\n';
                    source.first_inference_logged = true;
                }
                // Clean display copy taken BEFORE small-frame annotation, so the
                // canvas can paste pristine video pixel-perfect and redraw every
                // overlay at canvas resolution. Evidence keeps using the annotated
                // frame below; inference and telemetry order are unchanged.
                cv::Mat clean_for_display;
                if (config.show_window) clean_for_display = frame.clone();
                if (canonicalFrameNeedsRender(config.show_window, processed.canonical)) {
                    const auto render_started = telemetry == nullptr ? Clock::time_point{} : Clock::now();
                    const float keypoint_threshold = std::clamp(config.pose_confidence, 0.25F, 0.50F);
                    for (const auto& person : processed.canonical.people) {
                        if (config.analytics_mode == AnalyticsMode::PpeFall) drawPose(frame, person.keypoints, keypoint_threshold);
                        drawPerson(frame, person);
                        const auto& association = processed.associations.at(person.track_id);
                        for (const PpeItem item : requiredPpeItems()) {
                            if (!config.ppe_enabled[static_cast<std::size_t>(item)]) continue;
                            drawAssociatedItem(frame, association.detection(item), std::string(ppeItemLabel(item)));
                        }
                    }
                    if (telemetry != nullptr) telemetry->addSample(PerformanceStage::Render, Clock::now() - render_started);
                    if (config.show_window) {
                        source.last_people = processed.canonical.people;
                        source.last_associations = processed.associations;
                        source.last_keypoint_threshold = keypoint_threshold;
                        source.last_analytics_mode = config.analytics_mode;
                    }
                } else if (config.show_window) {
                    source.last_people.clear();
                    source.last_associations.clear();
                }
                EvidenceWriterQueue::AnnotatedFrame queued_frame;
                if (evidence_queue && !processed.canonical.events.empty()) {
                    queued_frame = EvidenceWriterQueue::cloneAnnotatedFrame(frame);
                }
                for (const auto& event : processed.canonical.events) {
                    if (evidence_queue) {
                        if (!evidence_queue->enqueue(queued_frame, source.config->label, event)) {
                            std::cerr << "Evidence queue rejected " << event.id << ": "
                                      << evidence_queue->failureMessage() << '\n';
                            stop_requested.store(true, std::memory_order_relaxed);
                            break;
                        }
                        continue;
                    }
                    try {
                        if (telemetry != nullptr) telemetry->evidenceAppendAttempted();
                        const auto evidence_started = telemetry == nullptr ? Clock::time_point{} : Clock::now();
                        const auto record = evidence->append(frame, source.config->label, event);
                        if (event.type == "com.cuajone.safety.ppe.violation.v2") evidence_v3->append(event);
                        if (telemetry != nullptr) {
                            telemetry->addSample(PerformanceStage::EvidenceAppend, Clock::now() - evidence_started);
                            telemetry->evidenceAppendWritten();
                        }
                        std::cout << "Event [" << source.config->label << "]: " << record.event_type
                                  << " | track " << record.track_id << " | " << record.date << 'T' << record.time << "Z\n";
                    } catch (const std::exception& error) {
                        if (telemetry != nullptr) telemetry->evidenceAppendFailed();
                        std::cerr << "Evidence write failed: " << error.what() << '\n';
                    }
                }
                if (config.show_window) {
                    if (config.performance_report && telemetry != nullptr) {
                        telemetry->displayedFrame();
                        source.last_metrics = telemetry->overlayMetrics(frame.cols, frame.rows);
                        source.has_metrics = true;
                    } else if (!config.performance_report) {
                        source.has_metrics = false;
                    }
                    source.last_connected = true;
                    // Store the CLEAN frame; the single grid canvas is composed
                    // once per loop below and every overlay is redrawn there at
                    // canvas resolution. A failed decode never clears this
                    // slot, so the grid keeps the last good tile.
                    source.last_displayed_frame = clean_for_display.empty() ? frame : clean_for_display;
                    source.display_dirty = true;
                }
            }
        }
        if (config.show_window) {
            // Refresh while dragging, on weight changes (mouse or '+/-' keys),
            // and while a hover label is visible so it hides on timeout.
            // Otherwise compose only on new frames. Composition runs only
            // when show_window is set; headless runs never allocate a canvas.
            const auto now_for_dirty = Clock::now();
            const bool hover_visible = mosaic.hover_tile >= 0
                && mosaic.hover_time != Clock::time_point::min()
                && now_for_dirty - mosaic.hover_time < kHoverLabelTimeout + std::chrono::milliseconds(500);
            const bool weights_changed = mosaic.col_weights != last_composed_cols
                || mosaic.row_weights != last_composed_rows;
            const bool grid_dirty = !grid_shown || mosaic.dragging || weights_changed || hover_visible
                || std::ranges::any_of(sources, [](const SourceState& source) {
                        return source.display_dirty;
                    });
            if (grid_dirty && grid_layout.cols > 0) {
                mosaic.tile_rects = weightedTileRects(
                    grid_layout, mosaic.col_weights, mosaic.row_weights,
                    canvas_width, canvas_height);
                // Center the grid on the fixed 16:9 display canvas. Tile rects
                // are stored in display coordinates so hover, dividers, focus
                // and mouse rebalance keep working unchanged (offsets cancel
                // in the fraction math).
                const int display_dx = std::max(0, (kDisplayWidth - canvas_width) / 2);
                const int display_dy = std::max(0, (kDisplayHeight - canvas_height) / 2);
                for (auto& rect : mosaic.tile_rects) {
                    rect.x += display_dx;
                    rect.y += display_dy;
                }
                cv::Mat display(kDisplayHeight, kDisplayWidth, CV_8UC3, cv::Scalar(0, 0, 0));
                const auto compose_time = Clock::now();
                for (std::size_t index = 0; index < sources.size()
                    && index < mosaic.tile_rects.size(); ++index) {
                    auto& source = sources[index];
                    const cv::Rect tile = mosaic.tile_rects[index];
                    const cv::Mat clean = source.last_displayed_frame.empty()
                        ? cv::Mat(720, 1280, CV_8UC3, cv::Scalar(24, 24, 24))
                        : source.last_displayed_frame;
                    const LetterboxMap mapping = pasteLetterboxed(display, tile, clean);
                    if (!source.last_people.empty()) {
                        drawDetectionsOnCanvas(display, mapping, source.last_people,
                            source.last_associations, source.last_analytics_mode,
                            source.last_keypoint_threshold);
                    }
                    if (config.performance_report && source.has_metrics) {
                        drawPerformancePanelOnCanvas(
                            display, tile, source.last_metrics, source.last_connected);
                    }
                    if (!source.last_connected) drawReconnectBannerOnCanvas(display, tile);
                    // Camera name auto-hides; it is drawn at canvas resolution
                    // only on the hovered tile while the hover is fresh (2 s
                    // timeout, dimmed during the second half as a fade hint).
                    // Without mouse movement there are no labels at all.
                    if (static_cast<int>(index) == mosaic.hover_tile
                        && mosaic.hover_time != Clock::time_point::min()
                        && compose_time - mosaic.hover_time < kHoverLabelTimeout) {
                        const bool fading =
                            compose_time - mosaic.hover_time > kHoverLabelTimeout / 2;
                        const cv::Scalar color =
                            fading ? cv::Scalar(180, 180, 180) : cv::Scalar(255, 255, 255);
                        const std::string label = source.config->label;
                        int baseline = 0;
                        const cv::Size text_size = cv::getTextSize(
                            label, cv::FONT_HERSHEY_SIMPLEX, 0.7, 2, &baseline);
                        const cv::Rect tag(
                            tile.x + 8, tile.y + 8, text_size.width + 16,
                            text_size.height + baseline + 12);
                        const cv::Rect clipped = tag & cv::Rect(0, 0, display.cols, display.rows);
                        if (!clipped.empty()) {
                            cv::Mat tag_pixels = display(clipped);
                            cv::Mat tag_bg(
                                tag_pixels.size(), tag_pixels.type(), cv::Scalar(0, 0, 0));
                            cv::addWeighted(tag_bg, 0.65, tag_pixels, 0.35, 0.0, tag_pixels);
                            cv::putText(display, label, {tag.x + 8, tag.y + text_size.height + 6},
                                cv::FONT_HERSHEY_SIMPLEX, 0.7, color, 2, cv::LINE_AA);
                        }
                    }
                    // Focused-tile frame so keyboard resizing has a visible target.
                    if (static_cast<int>(index) == mosaic.focused_tile
                        && mosaic.tile_rects.size() > 1) {
                        cv::rectangle(display, tile, cv::Scalar(0, 200, 255), 1);
                    }
                    source.display_dirty = false;
                }
                cv::putText(display,
                    "Drag divider: resize | +/-: focused tile | [/]: focus | 0: reset | hover: name | q/Esc: quit",
                    {display_dx + 12, display.rows - 10}, cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(220, 220, 220), 1, cv::LINE_AA);
                cv::imshow(kLiveAnalyticsWindowTitle, display);
                grid_shown = true;
                last_composed_cols = mosaic.col_weights;
                last_composed_rows = mosaic.row_weights;
            }
        }
        if (!any_running) break;
        if (config.show_window && pollLiveWindow(mosaic, kLiveAnalyticsWindowTitle)) {
            stop_requested.store(true, std::memory_order_relaxed);
        } else if (!received_any && !config.show_window) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    for (auto& source : sources) source.capture->stop();
    cv::destroyAllWindows();
    if (!evidence_queue) return 0;
    evidence_queue->drainAndStop();
    const EvidenceWriterQueueStats queue_stats = evidence_queue->stats();
    if (telemetry != nullptr) {
        for (std::uint64_t index = 0; index < queue_stats.accepted; ++index) {
            telemetry->evidenceAppendAttempted();
        }
        for (std::uint64_t index = 0; index < queue_stats.written; ++index) {
            telemetry->evidenceAppendWritten();
        }
        for (std::uint64_t index = 0; index < queue_stats.failed; ++index) {
            telemetry->evidenceAppendFailed();
        }
        telemetry->setEvidenceQueueTelemetry({
            true,
            queue_stats.capacity,
            queue_stats.accepted,
            queue_stats.written,
            queue_stats.failed,
            queue_stats.current_depth,
            queue_stats.high_water_depth,
            queue_stats.blocked_enqueue_count,
            queue_stats.blocked_enqueue_duration,
            queue_stats.drain_duration,
            queue_stats.terminal_failure,
        });
    }
    if (queue_stats.terminal_failure) {
        std::cerr << "Evidence queue failed: " << evidence_queue->failureMessage() << '\n';
        return 1;
    }
    return 0;
}

std::string benchmarkObservedAt(std::uint64_t frame_id) {
    const std::uint64_t seconds = frame_id / 1000;
    const std::uint64_t milliseconds = frame_id % 1000;
    std::ostringstream output;
    output << "1970-01-01T00:00:" << std::setfill('0') << std::setw(2) << seconds
           << '.' << std::setw(3) << milliseconds << 'Z';
    return output.str();
}

int benchmark(
    const RuntimeConfig& config,
    NativeEnginePipeline& pipeline,
    PerformanceTelemetry& telemetry) {
    const cv::Mat image = cv::imread(config.benchmark_image.string(), cv::IMREAD_COLOR);
    if (image.empty()) throw std::runtime_error("Could not decode benchmark image");
    telemetry.setBenchmarkMetadata({
        config.benchmark_warmup,
        config.benchmark_iterations,
        image.cols,
        image.rows,
    });
    runBenchmarkIterations(
        config.benchmark_warmup, config.benchmark_iterations, telemetry,
        [&](std::size_t iteration) {
            const std::uint64_t frame_id = static_cast<std::uint64_t>(iteration) + 1;
            const auto timestamp_ms = static_cast<std::int64_t>(frame_id);
            static_cast<void>(pipeline.processFrame(
                image, "benchmark-image", frame_id, timestamp_ms, benchmarkObservedAt(frame_id)));
            telemetry.processedFrame();
        },
        [](const BenchmarkProgress& progress) {
            if (progress.warmup_complete) {
                std::cout << "Benchmark progress: warmup complete; measured frames 0/"
                          << progress.measured_iterations << '\n';
                return;
            }
            std::cout << "Benchmark progress: measured frames "
                      << progress.completed_measured_frames << '/'
                      << progress.measured_iterations << '\n';
        });
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
#ifdef _WIN32
    std::signal(SIGBREAK, requestStop);
#endif
    try {
        RuntimeConfig config = parseCommandLine(argc, argv);
        if (config.help) {
            printHelp(std::cout);
            return 0;
        }
        if (config.hardware_probe_json) {
            const HardwareProbeResult probe = probeHardware();
            std::cout << hardwareProbeJson(probe) << '\n';
            return hardwareProbeExitCode(probe.status);
        }
        std::optional<ComputeBackend> installed_backend;
        if (!config.compute_explicit) installed_backend = installedComputeBackend();
        const ComputeBackend requested_backend = resolveRequestedComputeBackend(
            config.compute_backend, config.compute_explicit, installed_backend);
        HardwareProbeStatus hardware_status = HardwareProbeStatus::NoNvidiaAdapter;
        if (requested_backend != ComputeBackend::Cpu) {
            const HardwareProbeResult probe = probeHardware();
            hardware_status = probe.status;
            std::cout << "Hardware probe: " << hardwareProbeStatusName(probe.status)
                      << " | " << hardwareProbeSummary(probe)
                      << " | " << probe.detail << '\n';
        }
        const RuntimeExecutionPlan plan = planRuntimeExecution({
            config.compute_backend,
            config.compute_explicit,
            installed_backend,
            config.analytics_mode,
            {
                hardware_status,
                tensorRtBackendCompiled(),
                onnxCudaExecutionProviderCompiled(),
            },
            modelArtifactAvailability(config),
        });
        std::cout << "Compute: " << computeBackendName(plan.selection.backend)
                  << " | " << plan.selection.reason << '\n';
        std::unique_ptr<NativeEnginePipeline> pipeline;
        std::unique_ptr<PerformanceTelemetry> telemetry;
        if (config.performance_report) {
            telemetry = std::make_unique<PerformanceTelemetry>(
                config.benchmark_image.empty() ? performanceSourceMode(config.source) : "benchmark-image");
            telemetry->setEvidenceQueueTelemetry({
                config.evidence_writer_queue_capacity != 0,
                config.evidence_writer_queue_capacity,
            });
        }
        ComputeSelection effective_selection = plan.selection;
        try {
            pipeline = runBasePreflight(config, effective_selection, telemetry.get());
        } catch (const std::exception& cuda_error) {
            if (!plan.preflight_failure_fallback) {
                throw;
            }
            std::cerr << "Auto CUDA validation failed; selecting CPU: " << cuda_error.what() << '\n';
            effective_selection = *plan.preflight_failure_fallback;
            pipeline = runBasePreflight(config, effective_selection, telemetry.get());
        }
        std::cout << "Preflight: OK\n";
#ifdef _WIN32
        if (isCudaWarmupChild()) return 0;
#endif
        if (config.preflight) return 0;
        const int result = config.benchmark_image.empty()
            ? monitor(config, *pipeline, telemetry.get())
            : benchmark(config, *pipeline, *telemetry);
        if (telemetry) std::cout << telemetry->jsonReport() << '\n';
        return result;
    } catch (const std::invalid_argument& error) {
        std::cerr << "Configuration error: " << error.what() << "\nUse --help for usage.\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Runtime error: " << error.what() << '\n';
        return 1;
    }
}
