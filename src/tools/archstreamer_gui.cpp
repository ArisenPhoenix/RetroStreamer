#include "main_window.hpp"

#include "client/remoted_keyboard_source.hpp"
#include "gui_logging.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QIcon>
#include <QKeyEvent>
#include <QPixmap>
#include <QTimer>

#include <iostream>

namespace {

QIcon archstreamer_app_icon() {
    // Prefer the embedded Qt resource (works from any cwd).
    const QPixmap from_qrc(QStringLiteral(":/branding/archstreamer-icon-256.png"));
    if (!from_qrc.isNull()) {
        return QIcon(from_qrc);
    }
    // Fallbacks for incomplete builds / tooling.
    const QStringList candidates = {
        QStringLiteral("branding/archstreamer-icon-256.png"),
        QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QStringLiteral("../branding/archstreamer-icon-256.png")),
        QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QStringLiteral("branding/archstreamer-icon-256.png")),
    };
    for (const QString& path : candidates) {
        if (QFile::exists(path)) {
            return QIcon(path);
        }
    }
    return {};
}

class RemotedKeyboardEventFilter final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        (void)watched;
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) {
            return false;
        }
        const auto* key_event = static_cast<const QKeyEvent*>(event);
        if (key_event->isAutoRepeat()) {
            return false;
        }
        const auto bit = archstreamer::remoted_key_bit_from_qt_key(key_event->key());
        if (bit == 0) {
            return false;
        }
        auto& gui_keys = archstreamer::GuiFocusRemotedKeyboardSource::instance();
        auto keys = gui_keys.poll_keys();
        if (event->type() == QEvent::KeyPress) {
            keys |= bit;
        } else {
            keys &= ~bit;
        }
        gui_keys.set_keys(keys);
        return false;
    }
};

} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    using archstreamer::gui::log_timestamp;
    using archstreamer::gui::mirror_gui_logs_to_stdout;
    using archstreamer::gui::write_to_log_file;
    using archstreamer::gui::gui_log_path;

    write_to_log_file("[" + log_timestamp().toStdString() + "] === archstreamer_gui started ===");
    write_to_log_file("[" + log_timestamp().toStdString() + "] Log file: " + gui_log_path().string());

    // Qt installs a SIGINT handler that calls quit(); keep that. Avoid anything that
    // would run heavy GStreamer teardown on the signal itself.

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ArchStreamer"));
    app.setOrganizationName(QStringLiteral("ArchStreamer"));
    // Wayland/GNOME ignore setWindowIcon and resolve the mark via the desktop file.
    app.setDesktopFileName(QStringLiteral("io.github.ArisenPhoenix.ArchStreamer"));
    const QIcon app_icon = archstreamer_app_icon();
    app.setWindowIcon(app_icon);
    RemotedKeyboardEventFilter keyboard_filter;
    app.installEventFilter(&keyboard_filter);
    archstreamer::gui::MainWindow window;
    window.setWindowIcon(app_icon);
    window.show();

    for (int index = 1; index + 1 < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index]) == "--debug-profile") {
            mirror_gui_logs_to_stdout = true;
            const auto profile = QString::fromLocal8Bit(argv[index + 1]);
            QTimer::singleShot(0, &window, [&window, profile] {
                window.apply_debug_profile(profile);
            });
            break;
        }
    }

    return app.exec();
}
