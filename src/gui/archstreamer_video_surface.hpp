#pragma once

#include "client/video_embed_bridge.hpp"

#include <QImage>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

class QPaintEngine;
class QResizeEvent;

namespace archstreamer::gui {

/**
 * Reusable video host widget. Receives RGBA frames from VideoEmbedBridge (appsink).
 */
class ArchStreamerVideoSurface final : public QWidget {
    Q_OBJECT

public:
    explicit ArchStreamerVideoSurface(QWidget* parent = nullptr);

    /** Kept for diagnostics; appsink path does not need a native xid. */
    quint64 embedXid() const;

    void setFrameBridge(std::shared_ptr<archstreamer::VideoEmbedBridge> bridge);

    /** Pull latest frame from the bridge (call from a GUI timer). */
    void refreshFromBridge();

    /** Allow close without emitting userClosed (session already tearing down). */
    void forceClose();

signals:
    /** User hit the window close button; session should stop gst then leave. */
    void userClosed();
    void geometryChanged(int width, int height);
    void exposeNeeded();

protected:
    void closeEvent(QCloseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void emitGeometry();

    bool force_close_ = false;
    bool shutdown_started_ = false;
    std::shared_ptr<archstreamer::VideoEmbedBridge> frame_bridge_;
    QImage frame_;
    std::vector<std::uint8_t> frame_bytes_;
    std::uint64_t frame_serial_ = 0;
};

QString make_video_window_title(const QString& system_name, const QString& game_name);

} // namespace archstreamer::gui
