// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <opencv2/core.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class QApplication;

namespace cuajone {

struct QtMosaicGridLayout {
    std::size_t rows{};
    std::size_t cols{};
};

// UI state is deliberately separate from inference state. The monitor owns
// this object and composes the same canvas used by the Windows viewer; Qt only
// owns window events and presentation.
struct QtMosaicUiState {
    QtMosaicGridLayout layout{};
    std::vector<double> col_weights;
    std::vector<double> row_weights;
    std::vector<cv::Rect> tile_rects;
    int canvas_width{};
    int canvas_height{};
    int hover_tile{-1};
    std::chrono::steady_clock::time_point hover_time{
        std::chrono::steady_clock::time_point::min()};
    int focused_tile{};
    bool dragging{};
    int drag_col{-1};
    int drag_row{-1};
    bool ui_dirty{};
};

class QtMosaicViewer final {
public:
    QtMosaicViewer(int& argc, char** argv, std::string_view title, QtMosaicUiState& state);
    ~QtMosaicViewer();

    QtMosaicViewer(const QtMosaicViewer&) = delete;
    QtMosaicViewer& operator=(const QtMosaicViewer&) = delete;

    void setTileRects(const std::vector<cv::Rect>& tile_rects);
    void setCanvas(const cv::Mat& canvas);

    // Pumps the existing QApplication without entering a second event loop.
    // Returns true when the user closed the window or pressed Q/Escape.
    [[nodiscard]] bool processEvents();

private:
    class MosaicWidget;

    std::unique_ptr<QApplication> application_;
    std::unique_ptr<MosaicWidget> widget_;
    QtMosaicUiState& state_;
    bool quit_requested_{};
};

}  // namespace cuajone
