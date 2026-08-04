#include "client_video_controller.hpp"

#include <QApplication>
#include <QTimer>

namespace archstreamer::gui {

ClientVideoController::ClientVideoController(QObject* parent)
    : QObject(parent) {
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(16);
    QObject::connect(refresh_timer_, &QTimer::timeout, this, [this] {
        if (surface_) {
            surface_->refreshFromBridge();
        }
    });
}

ClientVideoController::~ClientVideoController() {
    endSession();
}

void ClientVideoController::setMode(ClientVideoMode mode) {
    mode_ = mode;
    if (surface_) {
        applyMode();
    }
}

void ClientVideoController::setTitleFromGame(
    const QString& system_name,
    const QString& game_name) {
    title_ = make_video_window_title(system_name, game_name);
    if (surface_) {
        surface_->setWindowTitle(title_);
    }
}

void ClientVideoController::setEmbedParent(QWidget* parent) {
    embed_parent_ = parent;
    if (surface_ && mode_ == ClientVideoMode::Embedded) {
        applyMode();
    }
}

void ClientVideoController::setVideoEmbedBridge(
    std::shared_ptr<archstreamer::VideoEmbedBridge> bridge) {
    embed_bridge_ = std::move(bridge);
    if (surface_) {
        surface_->setFrameBridge(embed_bridge_);
        if (embed_bridge_) {
            pushGeometryToBridge(surface_->width(), surface_->height());
        }
    }
}

void ClientVideoController::setHeartbeatPrefs(
    std::shared_ptr<archstreamer::ClientHeartbeatPrefs> prefs) {
    heartbeat_prefs_ = std::move(prefs);
    if (surface_ && heartbeat_prefs_) {
        pushGeometryToBridge(surface_->width(), surface_->height());
    }
}

void ClientVideoController::setDsTouchBridge(
    std::shared_ptr<archstreamer::DsTouchBridge> bridge) {
    ds_touch_ = std::move(bridge);
    if (surface_) {
        surface_->setDsTouchBridge(ds_touch_);
    }
}

void ClientVideoController::prepareForSession() {
    if (!surface_) {
        surface_ = std::make_unique<ArchStreamerVideoSurface>();
        QObject::connect(
            surface_.get(),
            &ArchStreamerVideoSurface::userClosed,
            this,
            &ClientVideoController::userClosed);
        QObject::connect(
            surface_.get(),
            &ArchStreamerVideoSurface::geometryChanged,
            this,
            [this](int w, int h) { pushGeometryToBridge(w, h); });
        QObject::connect(
            surface_.get(),
            &ArchStreamerVideoSurface::exposeNeeded,
            this,
            [this] {
                if (embed_bridge_) {
                    embed_bridge_->request_expose();
                }
            });
    }
    surface_->setFrameBridge(embed_bridge_);
    surface_->setDsTouchBridge(ds_touch_);
    surface_->setWindowTitle(title_);
    applyMode();
    surface_->show();
    surface_->raise();
    surface_->activateWindow();
    qApp->processEvents();
    pushGeometryToBridge(surface_->width(), surface_->height());
    refresh_timer_->start();
}

void ClientVideoController::endSession() {
    if (refresh_timer_ != nullptr) {
        refresh_timer_->stop();
    }
    fullscreen_suspended_for_overlay_ = false;
    // Never call GStreamer from here — this runs on the GUI/X11 thread.
    // Media is already stopped on the session worker before endSession is queued.
    if (embed_bridge_) {
        embed_bridge_->clear_emergency_stop();
    }
    if (!surface_) {
        return;
    }
    surface_->forceClose();
    surface_.reset();
}

std::uint64_t ClientVideoController::embedXid() const {
    if (!surface_) {
        return 0;
    }
    return static_cast<std::uint64_t>(surface_->embedXid());
}

void ClientVideoController::raiseVideo() {
    if (!surface_) {
        return;
    }
    if (mode_ == ClientVideoMode::FullScreen && !fullscreen_suspended_for_overlay_) {
        surface_->showFullScreen();
    } else {
        surface_->show();
    }
    surface_->raise();
    surface_->activateWindow();
}

void ClientVideoController::suspendFullScreenForOverlay() {
    if (!surface_ || mode_ != ClientVideoMode::FullScreen) {
        return;
    }
    if (!surface_->isFullScreen() && !fullscreen_suspended_for_overlay_) {
        return;
    }
    fullscreen_suspended_for_overlay_ = true;
    // Windowed so sibling Qt dialogs (pad OSK) can stack above the video surface.
    surface_->showNormal();
    surface_->show();
}

void ClientVideoController::resumeFullScreenAfterOverlay() {
    if (!fullscreen_suspended_for_overlay_) {
        return;
    }
    fullscreen_suspended_for_overlay_ = false;
    if (!surface_ || mode_ != ClientVideoMode::FullScreen) {
        return;
    }
    surface_->showFullScreen();
    surface_->raise();
    surface_->activateWindow();
}

void ClientVideoController::pushGeometryToBridge(int width, int height) {
    if (embed_bridge_) {
        embed_bridge_->set_size(width, height);
    }
    if (heartbeat_prefs_ && width > 0 && height > 0) {
        heartbeat_prefs_->set_display_layout(
            width >= height ? archstreamer::DisplayLayoutPreference::Landscape
                            : archstreamer::DisplayLayoutPreference::Portrait);
    }
}

void ClientVideoController::applyMode() {
    if (!surface_) {
        return;
    }
    switch (mode_) {
    case ClientVideoMode::Embedded:
        if (embed_parent_ != nullptr) {
            surface_->setParent(embed_parent_);
            surface_->setWindowFlags(Qt::Widget);
            surface_->show();
        } else {
            surface_->setParent(nullptr);
            surface_->setWindowFlags(Qt::Window);
            surface_->show();
        }
        break;
    case ClientVideoMode::TopLevel:
        surface_->setParent(nullptr);
        surface_->setWindowFlags(Qt::Window);
        surface_->showNormal();
        break;
    case ClientVideoMode::FullScreen:
        surface_->setParent(nullptr);
        surface_->setWindowFlags(Qt::Window);
        surface_->showFullScreen();
        break;
    }
}

} // namespace archstreamer::gui
