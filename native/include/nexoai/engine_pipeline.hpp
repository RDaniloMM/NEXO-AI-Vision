// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "nexoai/analytics_pipeline.hpp"
#include "nexoai/compute.hpp"
#include "nexoai/inference_settings.hpp"

#include <opencv2/core/mat.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nexoai {

class PerformanceTelemetry;

struct EnginePipelineConfig {
    ComputeBackend backend{ComputeBackend::Cuda};
    InferenceProvider provider{InferenceProvider::TensorRt};
    std::filesystem::path ppe_engine;
    std::filesystem::path pose_engine;
    std::filesystem::path ppe_onnx;
    std::filesystem::path pose_onnx;
    std::optional<std::map<int, std::string>> ppe_labels;
    std::size_t pose_class_count{1};
    std::array<int, 2> pose_keypoint_shape{17, 3};
    bool allow_nonperson_pose_class{};
    // Safe default: pose can only affect analytics when PPE also found a
    // person, so empty scenes skip pose preprocess/inference/decode entirely.
    bool pose_requires_person{true};
    std::optional<int> device;
    int image_size{kDefaultImageSize};
    float ppe_confidence{0.10F};
    std::array<float, kPpeOutputLabels.size()> ppe_class_confidences{
        0.10F, 0.10F, 0.10F, 0.10F, 0.10F, 0.10F, 0.10F, 0.10F,
    };
    std::array<bool, kPpeItemCount> ppe_enabled{true, true, true, true, true, true, true};
    float pose_confidence{0.35F};
    float nms_iou{0.45F};
    std::size_t maximum_detections{300};
    AnalyticsPipelineConfig analytics;
    PerformanceTelemetry* telemetry{};
#ifdef NEXOAI_INTERNAL_DIAGNOSTICS
    // Compiled only into diagnostic targets to compare the hybrid serial reference.
    bool force_serial_hybrid{};
    // Compiled only into diagnostic targets to benchmark the legacy separate preprocess path.
    bool force_separate_hybrid_preprocessing{};
    // Compiled only into diagnostic targets to compare the sequential TensorRT reference.
    bool force_serial_tensorrt{};
#endif
};

struct EnginePipelineSummary {
    ComputeBackend backend{ComputeBackend::Cpu};
    std::string provider;
    std::string pose_provider;
    std::string device_name;
    int device_index{};
    int device_count{};
    int compute_major{};
    int compute_minor{};
    bool ppe_metadata_prefix{};
    bool pose_loaded{};
    bool pose_metadata_prefix{};
    bool pose_requires_person{};
    int image_size{kDefaultImageSize};
    std::size_t maximum_batch_size{1};
};

struct EngineFrameInput {
    cv::Mat frame;
    std::string source_id;
    std::uint64_t frame_id{};
    std::int64_t monotonic_timestamp_ms{};
    std::string observed_at;
};

[[nodiscard]] inline bool shouldRunPoseInference(
    bool pose_loaded,
    bool pose_requires_person,
    std::span<const Detection> ppe_detections,
    std::span<const int> person_class_ids) noexcept {
    if (!pose_loaded) return false;
    if (!pose_requires_person) return true;
    return std::ranges::any_of(ppe_detections, [&](const Detection& detection) {
        return std::ranges::find(person_class_ids, detection.class_id) != person_class_ids.end();
    });
}

class NativeEnginePipeline {
public:
    explicit NativeEnginePipeline(EnginePipelineConfig config);
    ~NativeEnginePipeline();
    NativeEnginePipeline(const NativeEnginePipeline&) = delete;
    NativeEnginePipeline& operator=(const NativeEnginePipeline&) = delete;

    ProcessedFrame processFrame(
        const cv::Mat& bgr_frame,
        std::string source_id,
        std::uint64_t frame_id,
        std::int64_t monotonic_timestamp_ms,
        std::string observed_at);
    std::vector<ProcessedFrame> processBatch(std::span<const EngineFrameInput> frames);
    void reset() noexcept;
    [[nodiscard]] const EnginePipelineSummary& summary() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool tensorRtBackendCompiled() noexcept;
bool onnxCudaExecutionProviderCompiled() noexcept;

}  // namespace nexoai
