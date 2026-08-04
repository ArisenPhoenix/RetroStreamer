#include "archstreamer_video_surface.hpp"

#include "common/ds_touch_mapping.hpp"

#include <QCloseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>

namespace archstreamer::gui {

ArchStreamerVideoSurface::ArchStreamerVideoSurface(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setMinimumSize(320, 180);
    resize(1280, 720);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, Qt::black);
    setPalette(palette);
}

quint64 ArchStreamerVideoSurface::embedXid() const {
    return static_cast<quint64>(winId());
}

void ArchStreamerVideoSurface::setFrameBridge(
    std::shared_ptr<archstreamer::VideoEmbedBridge> bridge) {
    frame_bridge_ = std::move(bridge);
}

void ArchStreamerVideoSurface::setDsTouchBridge(
    std::shared_ptr<archstreamer::DsTouchBridge> bridge) {
    ds_touch_ = std::move(bridge);
}

void ArchStreamerVideoSurface::refreshFromBridge() {
    if (!frame_bridge_) {
        return;
    }
    int w = 0;
    int h = 0;
    int stride = 0;
    if (!frame_bridge_->copy_frame(frame_bytes_, w, h, stride, frame_serial_)) {
        return;
    }
    // QImage does not own the buffer — keep frame_bytes_ alive.
    frame_ = QImage(
        frame_bytes_.data(),
        w,
        h,
        stride,
        QImage::Format_RGBA8888);
    update();
}

void ArchStreamerVideoSurface::forceClose() {
    force_close_ = true;
    close();
}

void ArchStreamerVideoSurface::closeEvent(QCloseEvent* event) {
    if (force_close_) {
        event->accept();
        return;
    }
    // Keep the widget alive until the session thread tears media down.
    // Do not touch GStreamer here — even async set_state on the GUI/X11 thread
    // still hitches the desktop briefly.
    event->ignore();
    if (!shutdown_started_) {
        shutdown_started_ = true;
        hide();
        emit userClosed();
    }
}

void ArchStreamerVideoSurface::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (frame_.isNull()) {
        return;
    }
    // Letterbox into the widget.
    const QSize target = frame_.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect dest(
        (width() - target.width()) / 2,
        (height() - target.height()) / 2,
        target.width(),
        target.height());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(dest, frame_);
}

void ArchStreamerVideoSurface::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    emitGeometry();
}

void ArchStreamerVideoSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    emitGeometry();
}

void ArchStreamerVideoSurface::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        handleDsTouch(event->position(), true);
    }
    QWidget::mousePressEvent(event);
}

void ArchStreamerVideoSurface::mouseMoveEvent(QMouseEvent* event) {
    if (ds_touching_ && (event->buttons() & Qt::LeftButton)) {
        handleDsTouch(event->position(), true);
    }
    QWidget::mouseMoveEvent(event);
}

void ArchStreamerVideoSurface::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && ds_touching_) {
        handleDsTouch(event->position(), false);
        ds_touching_ = false;
    }
    QWidget::mouseReleaseEvent(event);
}

void ArchStreamerVideoSurface::handleDsTouch(const QPointF& pos, bool pressed) {
    if (!ds_touch_) {
        return;
    }
    // Prefer host SCREENS when present (follows EmphTop ↔ EmphBot); else heuristic.
    const bool portrait_stack = height() > width();
    archstreamer::DsScreenRects rects{};
    const auto layout = ds_touch_->snapshot_layout();
    const archstreamer::DsScreenRects* layout_ptr = nullptr;
    if (layout.has_value()) {
        rects.window_w = layout->window_w;
        rects.window_h = layout->window_h;
        rects.has_top = layout->has_top;
        rects.top_x = layout->top_x;
        rects.top_y = layout->top_y;
        rects.top_w = layout->top_w;
        rects.top_h = layout->top_h;
        rects.has_bot = layout->has_bot;
        rects.bot_x = layout->bot_x;
        rects.bot_y = layout->bot_y;
        rects.bot_w = layout->bot_w;
        rects.bot_h = layout->bot_h;
        layout_ptr = &rects;
    }
    const auto hit = archstreamer::resolve_bottom_screen_hit_rect(
        static_cast<float>(width()),
        static_cast<float>(height()),
        portrait_stack,
        layout_ptr);
    if (!hit.valid()) {
        return;
    }
    float nx = 0.f;
    float ny = 0.f;
    if (!archstreamer::view_point_to_normalized(
            static_cast<float>(pos.x()),
            static_cast<float>(pos.y()),
            hit,
            nx,
            ny)) {
        if (!pressed && ds_touching_) {
            ds_touch_->push_sample(0, 0, false);
            ds_touching_ = false;
        }
        return;
    }
    std::uint16_t norm_x = 0;
    std::uint16_t norm_y = 0;
    archstreamer::encode_normalized_u16(nx, ny, norm_x, norm_y);
    ds_touch_->push_sample(norm_x, norm_y, pressed);
    ds_touching_ = pressed;
}

void ArchStreamerVideoSurface::emitGeometry() {
    const int w = width();
    const int h = height();
    if (w > 0 && h > 0) {
        emit geometryChanged(w, h);
    }
}

QString make_video_window_title(const QString& system_name, const QString& game_name) {
    QString title = QStringLiteral("ArchStreamer");
    const QString system = system_name.trimmed();
    const QString game = game_name.trimmed();
    if (!system.isEmpty()) {
        title += QLatin1Char(' ');
        title += system;
    }
    if (!game.isEmpty()) {
        title += QLatin1Char(' ');
        title += game;
    }
    return title;
}

} // namespace archstreamer::gui
