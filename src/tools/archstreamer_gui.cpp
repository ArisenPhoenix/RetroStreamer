#include "main_window.hpp"

#include "client/remoted_keyboard_source.hpp"
#include "gui_logging.hpp"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QTimer>

#include <iostream>

namespace {

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

    QApplication app(argc, argv);
    RemotedKeyboardEventFilter keyboard_filter;
    app.installEventFilter(&keyboard_filter);
    archstreamer::gui::MainWindow window;
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
