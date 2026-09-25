// SPDX-License-Identifier: AGPL-3.0-only

#include "nexoai/engine_reader.hpp"
#include "nexoai/tensorrt_runtime.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: nexoai_trt_smoke <engine>\n";
        return 2;
    }
    try {
        nexoai::selectCudaDevice(0);
        const auto file = nexoai::EngineFile::read(std::filesystem::path(argv[1]));
        nexoai::TensorRtSession session(file, file.metadata().image_size);
        std::cout << "TensorRT engine inspected: input " << session.inputWidth() << 'x'
                  << session.inputHeight() << '\n';

        const std::size_t elements = static_cast<std::size_t>(session.inputWidth())
            * static_cast<std::size_t>(session.inputHeight()) * 3;
        std::vector<float> input(elements);
        for (std::size_t index = 0; index < elements; ++index) {
            input[index] = std::sin(static_cast<float>(index) * 0.017F);
        }

        const nexoai::InferenceOutput synchronous = session.infer(input);
        const std::vector<float> synchronous_copy(
            synchronous.values.begin(), synchronous.values.end());
        session.submit(input);
        const nexoai::InferenceOutput asynchronous = session.collect();
        if (asynchronous.values.size() != synchronous_copy.size()) {
            std::cerr << "Parity mismatch: output length differs between infer() and submit()+collect()\n";
            return 1;
        }
        if (std::memcmp(asynchronous.values.data(), synchronous_copy.data(),
                synchronous_copy.size() * sizeof(float)) != 0) {
            std::cerr << "Parity mismatch: infer() and submit()+collect() produced different bytes\n";
            return 1;
        }
        std::cout << "TensorRT parity check passed: infer() == submit()+collect() ("
                  << synchronous_copy.size() << " floats)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
