#include "archstreamer_video_surface.hpp"

#include <QCloseEvent>
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
