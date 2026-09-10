// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace cuajone {

struct InferenceOutput {
    std::span<const float> values;
    std::span<const std::int64_t> shape;
};

class InferenceSession {
public:
    virtual ~InferenceSession() = default;
    [[nodiscard]] virtual int inputWidth() const noexcept = 0;
    [[nodiscard]] virtual int inputHeight() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<std::int64_t>& outputShape() const noexcept = 0;
    [[nodiscard]] virtual std::size_t maximumBatchSize() const noexcept { return 1; }
    virtual InferenceOutput infer(std::span<const float> nchw_input) = 0;
    virtual InferenceOutput inferBatch(std::span<const float> nchw_input, std::size_t batch_size) {
        if (batch_size != 1) throw std::invalid_argument("Inference session supports batch size 1 only");
        return infer(nchw_input);
    }
};

}  // namespace cuajone
