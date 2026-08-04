#include "gui_logging.hpp"

#include <QMetaObject>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QThread>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>

namespace archstreamer::gui {

std::atomic_bool mirror_gui_logs_to_stdout = false;
std::atomic<int> gui_log_level{static_cast<int>(GuiLogLevel::Normal)};

std::filesystem::path gui_log_path() {
    const auto dir = std::filesystem::temp_directory_path() / "archstreamer-logs";
    std::filesystem::create_directories(dir);
    return dir / "gui.log";
}

namespace {

std::ofstream& log_file() {
    static std::ofstream file(gui_log_path(), std::ios::app);
    return file;
}

bool is_retroarch_log_line(const QString& line) {
    const auto text = line.trimmed();
    if (text.isEmpty()) {
        return false;
    }
    static const char* const kTags[] = {
        "[INFO]",
        "[WARN]",
        "[ERROR]",
        "[DEBUG]",
        "[VERBOSE]",
        "[libretro",
        "[GLSL]",
        "[Vulkan]",
        "[GLCore]",
        "[Wayland]",
        "[DRM]",
        "[X11]",
        "[PulseAudio]",
        "[ALSA]",
        "[Joypad]",
        "[Config]",
        "[Environ]",
        "[Autoconf]",
        "[Input]",
        "[Audio]",
        "[Video]",
        "[Core]",
        "[Content]",
        "[State]",
        "[SRAM]",
        "[Savestate]",
        "[Playlist]",
        "[Threaded]",
        "[Fonts]",
        "[Menu]",
        "[Overrides]",
        "[Shaders]",
    };
    for (const char* tag : kTags) {
        if (text.contains(QLatin1String(tag))) {
            return true;
        }
    }
    return text.contains(QLatin1String("RetroArch "));
}

} // namespace

void write_to_log_file(const std::string& message) {
    auto& f = log_file();
    f << message << '\n';
    f.flush();
}

QString log_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&time_t));
    return QString::fromLatin1(ts);
}

void append_log(QPlainTextEdit* log, QString message, GuiLogLevel level) {
    if (static_cast<int>(level) > gui_log_level.load()) {
        return;
    }
    if (log != nullptr) {
        const auto name = log->objectName();
        if (name == QLatin1String("hostLog") && !message.startsWith("[host]")) {
            message = "[host] " + message;
        } else if (name == QLatin1String("clientLog") && !message.startsWith("[client]")) {
            message = "[client] " + message;
        } else if (name == QLatin1String("remoteLog") && !message.startsWith("[remote]")) {
            message = "[remote] " + message;
        }
    }
    message = QString("[%1] %2").arg(log_timestamp(), message);
    write_to_log_file(message.toStdString());
    if (mirror_gui_logs_to_stdout.load()) {
        std::cout << message.toStdString() << '\n';
    }
    if (log == nullptr) {
        return;
    }
    auto append = [log, message = std::move(message)] {
        log->appendPlainText(message);
        constexpr int kMaxLogBlocks = 2000;
        auto* doc = log->document();
        if (doc != nullptr && doc->blockCount() > kMaxLogBlocks) {
            QTextCursor cursor(doc);
            cursor.movePosition(QTextCursor::Start);
            cursor.movePosition(
                QTextCursor::Down,
                QTextCursor::KeepAnchor,
                doc->blockCount() - kMaxLogBlocks);
            cursor.removeSelectedText();
        }
    };
    if (QThread::currentThread() == log->thread()) {
        append();
        return;
    }
    QMetaObject::invokeMethod(log, append, Qt::QueuedConnection);
}

void append_host_process_log(QPlainTextEdit* log, const QString& line) {
    const auto trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    append_log(
        log,
        trimmed,
        is_retroarch_log_line(trimmed) ? GuiLogLevel::Verbose : GuiLogLevel::Normal);
}

} // namespace archstreamer::gui
