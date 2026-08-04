#pragma once

#include "archstreamer_video_surface.hpp"

#include "client/client_app.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>

class QTimer;
class QWidget;

namespace archstreamer::gui {

enum class ClientVideoMode {
    Embedded,   // reparent into a Client tab (future default dock)
    TopLevel,   // separate window (ships first)
    FullScreen, // top-level + showFullScreen
};

/**
 * Owns ArchStreamerVideoSurface placement and title. Session code reads embedXid()
 * for in-process GstVideoOverlay; close → userClosed for clean Stop ordering.
 */
class ClientVideoController final : public QObject {
    Q_OBJECT

public:
    explicit ClientVideoController(QObject* parent = nullptr);
    ~ClientVideoController() override;

    void setMode(ClientVideoMode mode);
    ClientVideoMode mode() const { return mode_; }

    void setTitleFromGame(const QString& system_name, const QString& game_name);
    void setEmbedParent(QWidget* parent); // Embedded mode host (nullable until tab exists)

    /** Shares live widget size with the session/media thread. */
    void setVideoEmbedBridge(std::shared_ptr<archstreamer::VideoEmbedBridge> bridge);

    /** Drive Hello/heartbeat display_layout from video window aspect. */
    void setHeartbeatPrefs(std::shared_ptr<archstreamer::ClientHeartbeatPrefs> prefs);
    void setDsTouchBridge(std::shared_ptr<archstreamer::DsTouchBridge> bridge);

    /** Show/raise the surface for a live session; creates native window handle. */
    void prepareForSession();
    /** Hide and force-close after gst has been stopped. */
    void endSession();

    std::uint64_t embedXid() const;
    void raiseVideo();

    /**
     * Leave exclusive fullscreen so a Qt overlay (pad OSK) can sit in front.
     * No-op unless mode is FullScreen and the surface is currently fullscreen.
     * Pair with resumeFullScreenAfterOverlay() when the overlay closes.
     */
    void suspendFullScreenForOverlay();
    void resumeFullScreenAfterOverlay();

    ArchStreamerVideoSurface* surface() const { return surface_.get(); }

signals:
    void userClosed();

private:
    void applyMode();
    void pushGeometryToBridge(int width, int height);

    ClientVideoMode mode_ = ClientVideoMode::TopLevel;
    QString title_ = QStringLiteral("ArchStreamer");
    QWidget* embed_parent_ = nullptr;
    std::shared_ptr<archstreamer::VideoEmbedBridge> embed_bridge_;
    std::shared_ptr<archstreamer::ClientHeartbeatPrefs> heartbeat_prefs_;
    std::shared_ptr<archstreamer::DsTouchBridge> ds_touch_;
    std::unique_ptr<ArchStreamerVideoSurface> surface_;
    QTimer* refresh_timer_ = nullptr;
    /** True while an overlay (e.g. pad OSK) has forced us out of showFullScreen. */
    bool fullscreen_suspended_for_overlay_ = false;
};

} // namespace archstreamer::gui
