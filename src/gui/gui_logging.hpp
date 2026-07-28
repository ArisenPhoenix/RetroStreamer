#pragma once

#include <QString>

#include <atomic>
#include <filesystem>
#include <string>

class QPlainTextEdit;

namespace archstreamer::gui {

enum class GuiLogLevel : int {
    Quiet = 0,
    Normal = 1,
    Verbose = 2,
};

extern std::atomic_bool mirror_gui_logs_to_stdout;
extern std::atomic<int> gui_log_level;

std::filesystem::path gui_log_path();
void write_to_log_file(const std::string& message);
QString log_timestamp();

void append_log(QPlainTextEdit* log, QString message, GuiLogLevel level = GuiLogLevel::Normal);
void append_host_process_log(QPlainTextEdit* log, const QString& line);

} // namespace archstreamer::gui
