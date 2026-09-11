// SPDX-License-Identifier: AGPL-3.0-only

#include "qt_mosaic_viewer.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRectF>
#include <QEventLoop>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cuajone {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kDividerGrabPixels = 8;
constexpr double kMinTileWeight = 0.20;
constexpr double kFocusStepFactor = 1.10;
constexpr int kDisplayCanvasWidth = 1280;
constexpr int kDisplayCanvasHeight = 720;

QRectF canvasRectForSize(const QSize& size, int canvas_width, int canvas_height) {
    if (size.isEmpty() || canvas_width <= 0 || canvas_height <= 0) return {};
    const double scale = std::min({1.0,
        static_cast<double>(size.width()) / static_cast<double>(canvas_width),
        static_cast<double>(size.height()) / static_cast<double>(canvas_height)});
    const QSizeF fitted(canvas_width * scale, canvas_height * scale);
    return QRectF(
        (size.width() - fitted.width()) / 2.0,
        (size.height() - fitted.height()) / 2.0,
        fitted.width(), fitted.height());
}

void clampWeights(QtMosaicUiState& state) {
    for (double& weight : state.col_weights) {
        weight = std::clamp(weight, kMinTileWeight, 10.0);
    }
    for (double& weight : state.row_weights) {
        weight = std::clamp(weight, kMinTileWeight, 10.0);
    }
}

int tileAt(const QtMosaicUiState& state, int x, int y) {
    for (std::size_t index = 0; index < state.tile_rects.size(); ++index) {
        if (state.tile_rects[index].contains(cv::Point(x, y))) return static_cast<int>(index);
    }
    return -1;
}

void rebalanceColumns(QtMosaicUiState& state, int divider_col, int mouse_x) {
    if (divider_col < 0
        || static_cast<std::size_t>(divider_col + 1) >= state.col_weights.size()
        || state.tile_rects.empty()) return;
    const int left_edge = state.tile_rects[static_cast<std::size_t>(divider_col)].x;
    const int right_edge = state.tile_rects[static_cast<std::size_t>(divider_col + 1)].x
        + state.tile_rects[static_cast<std::size_t>(divider_col + 1)].width;
    const int span = std::max(1, right_edge - left_edge);
    const double fraction = std::clamp(
        static_cast<double>(mouse_x - left_edge) / static_cast<double>(span), 0.10, 0.90);
    const double total = state.col_weights[static_cast<std::size_t>(divider_col)]
        + state.col_weights[static_cast<std::size_t>(divider_col + 1)];
    state.col_weights[static_cast<std::size_t>(divider_col)] = std::max(kMinTileWeight, total * fraction);
    state.col_weights[static_cast<std::size_t>(divider_col + 1)] =
        std::max(kMinTileWeight, total * (1.0 - fraction));
    clampWeights(state);
}

void rebalanceRows(QtMosaicUiState& state, int divider_row, int mouse_y) {
    if (divider_row < 0
        || static_cast<std::size_t>(divider_row + 1) >= state.row_weights.size()
        || state.tile_rects.empty() || state.layout.cols == 0) return;
    const std::size_t row = static_cast<std::size_t>(divider_row);
    const int top_edge = state.tile_rects[row * state.layout.cols].y;
    const std::size_t next_row = static_cast<std::size_t>(divider_row + 1);
    const int bottom_edge = state.tile_rects[next_row * state.layout.cols].y
        + state.tile_rects[next_row * state.layout.cols].height;
    const int span = std::max(1, bottom_edge - top_edge);
    const double fraction = std::clamp(
        static_cast<double>(mouse_y - top_edge) / static_cast<double>(span), 0.10, 0.90);
    const double total = state.row_weights[row] + state.row_weights[next_row];
    state.row_weights[row] = std::max(kMinTileWeight, total * fraction);
    state.row_weights[next_row] = std::max(kMinTileWeight, total * (1.0 - fraction));
    clampWeights(state);
}

}  // namespace

class QtMosaicViewer::MosaicWidget final : public QWidget {
public:
    MosaicWidget(QtMosaicUiState& state, bool& quit_requested)
        : state_(state), quit_requested_(quit_requested) {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setMinimumSize(640, 360);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

    void setCanvas(const cv::Mat& canvas) {
        if (canvas.empty()) {
            image_ = {};
        } else if (canvas.type() == CV_8UC3) {
            // The cv::Mat belongs to the monitor and may be reused immediately
            // after this call. QImage therefore owns a deep copy.
            image_ = QImage(
                canvas.data, canvas.cols, canvas.rows, static_cast<int>(canvas.step),
                QImage::Format_BGR888).copy();
        } else {
            image_ = {};
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (image_.isNull()) return;
        const QRectF target = canvasRectForSize(size(), image_.width(), image_.height());
        painter.drawImage(target, image_);
    }

    void closeEvent(QCloseEvent* event) override {
        quit_requested_ = true;
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        const int key = event->key();
        if (key == Qt::Key_Q || key == Qt::Key_Escape) {
            quit_requested_ = true;
            event->accept();
            return;
        }
        if (state_.tile_rects.empty()) {
            event->ignore();
            return;
        }
        const std::size_t focus = static_cast<std::size_t>(std::clamp(
            state_.focused_tile, 0, static_cast<int>(state_.tile_rects.size()) - 1));
        if (key == Qt::Key_Plus || key == Qt::Key_Equal) {
            state_.col_weights[focus % state_.layout.cols] *= kFocusStepFactor;
            state_.row_weights[focus / state_.layout.cols] *= kFocusStepFactor;
            clampWeights(state_);
        } else if (key == Qt::Key_Minus || key == Qt::Key_Underscore) {
            state_.col_weights[focus % state_.layout.cols] /= kFocusStepFactor;
            state_.row_weights[focus / state_.layout.cols] /= kFocusStepFactor;
            clampWeights(state_);
        } else if (key == Qt::Key_BracketRight) {
            state_.focused_tile = (state_.focused_tile + 1)
                % static_cast<int>(state_.tile_rects.size());
        } else if (key == Qt::Key_BracketLeft) {
            state_.focused_tile = (state_.focused_tile - 1
                + static_cast<int>(state_.tile_rects.size()))
                % static_cast<int>(state_.tile_rects.size());
        } else if (key == Qt::Key_0) {
            std::fill(state_.col_weights.begin(), state_.col_weights.end(), 1.0);
            std::fill(state_.row_weights.begin(), state_.row_weights.end(), 1.0);
        } else {
            event->ignore();
            return;
        }
        state_.ui_dirty = true;
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        setFocus();
        const auto point = canvasPoint(event->position());
        if (!point) return;
        updateHover(*point);
        const int x = point->x;
        const int y = point->y;
        for (std::size_t col = 0; col + 1 < state_.layout.cols; ++col) {
            const int boundary = state_.tile_rects[col].x + state_.tile_rects[col].width;
            if (std::abs(x - boundary) <= kDividerGrabPixels) {
                state_.dragging = true;
                state_.drag_col = static_cast<int>(col);
                state_.drag_row = -1;
                state_.ui_dirty = true;
                grabMouse();
                return;
            }
        }
        for (std::size_t row = 0; row + 1 < state_.layout.rows; ++row) {
            const int boundary = state_.tile_rects[row * state_.layout.cols].y
                + state_.tile_rects[row * state_.layout.cols].height;
            if (std::abs(y - boundary) <= kDividerGrabPixels) {
                state_.dragging = true;
                state_.drag_row = static_cast<int>(row);
                state_.drag_col = -1;
                state_.ui_dirty = true;
                grabMouse();
                return;
            }
        }
        const int hit = tileAt(state_, x, y);
        if (hit >= 0) state_.focused_tile = hit;
        state_.ui_dirty = true;
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const auto point = canvasPoint(event->position());
        if (!point) return;
        updateHover(*point);
        if (!state_.dragging) return;
        if (state_.drag_col >= 0) rebalanceColumns(state_, state_.drag_col, point->x);
        else if (state_.drag_row >= 0) rebalanceRows(state_, state_.drag_row, point->y);
        state_.ui_dirty = true;
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            state_.dragging = false;
            state_.drag_col = -1;
            state_.drag_row = -1;
            state_.ui_dirty = true;
            releaseMouse();
        }
        event->accept();
    }

private:
    std::optional<cv::Point> canvasPoint(const QPointF& position) const {
        const QRectF target = canvasRectForSize(size(), image_.width(), image_.height());
        if (!target.contains(position) || target.width() <= 0.0 || target.height() <= 0.0) {
            return std::nullopt;
        }
        return cv::Point(
            static_cast<int>(std::floor((position.x() - target.left())
                * image_.width() / target.width())),
            static_cast<int>(std::floor((position.y() - target.top())
                * image_.height() / target.height())));
    }

    void updateHover(const cv::Point& point) {
        const int hit = tileAt(state_, point.x, point.y);
        if (hit >= 0) {
            state_.hover_tile = hit;
            state_.hover_time = Clock::now();
            state_.ui_dirty = true;
        }
    }

    QtMosaicUiState& state_;
    bool& quit_requested_;
    QImage image_;
};

QtMosaicViewer::QtMosaicViewer(
    int& argc, char** argv, std::string_view title, QtMosaicUiState& state)
    : application_(std::make_unique<QApplication>(argc, argv)), state_(state) {
    widget_ = std::make_unique<MosaicWidget>(state_, quit_requested_);
    widget_->setWindowTitle(QString::fromUtf8(title.data(), static_cast<int>(title.size())));
    widget_->resize(kDisplayCanvasWidth, kDisplayCanvasHeight);
    widget_->show();
    widget_->setFocus();
}

QtMosaicViewer::~QtMosaicViewer() {
    if (widget_) widget_->close();
    widget_.reset();
    application_.reset();
}

void QtMosaicViewer::setTileRects(const std::vector<cv::Rect>& tile_rects) {
    state_.tile_rects = tile_rects;
}

void QtMosaicViewer::setCanvas(const cv::Mat& canvas) {
    widget_->setCanvas(canvas);
}

bool QtMosaicViewer::processEvents() {
    application_->processEvents(QEventLoop::AllEvents, 5);
    return quit_requested_ || !widget_->isVisible();
}

}  // namespace cuajone
