#include "gui_host_runner.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include <filesystem>

namespace archstreamer::gui {

bool host_role_is_viewer(const QComboBox* combo) {
    return combo->currentData().toString() == QStringLiteral("viewer");
}

QString host_role_text(const QComboBox* combo) {
    return host_role_is_viewer(combo) ? QStringLiteral("viewer") : QStringLiteral("player");
}

QString host_runner_program() {
    if (qEnvironmentVariableIsSet("ARCHSTREAMER_HOST_RUNNER")) {
        const auto env = qEnvironmentVariable("ARCHSTREAMER_HOST_RUNNER");
        if (!env.isEmpty()) {
            return env;
        }
    }
    const auto app_dir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    const auto candidates = {
        app_dir / "host_runner",
        app_dir / "host_runner.exe",
        std::filesystem::current_path() / "build" / "host_runner",
        std::filesystem::current_path() / "build" / "Release" / "host_runner.exe",
        std::filesystem::current_path() / "build" / "host_runner.exe",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return QString::fromStdString(candidate.string());
        }
    }
    return QStringLiteral("./build/host_runner");
}

QString resolve_native_host_runner(const QString& configured) {
    if (!configured.trimmed().isEmpty() && QFileInfo::exists(configured.trimmed())) {
        return configured.trimmed();
    }
    if (const auto env = qEnvironmentVariable("ARCHSTREAMER_HOST_RUNNER"); !env.isEmpty()) {
        if (QFileInfo::exists(env)) {
            return env;
        }
    }
    const QString home = QDir::homePath();
    const QStringList candidates = {
        home + QStringLiteral("/.local/bin/host_runner"),
        home + QStringLiteral("/ArchStreamer-src/build-native/host_runner"),
        home + QStringLiteral("/Programming/Mixed/ArchStreamer/build/host_runner"),
        home + QStringLiteral("/src/ArchStreamer/build/host_runner"),
        QStringLiteral("/usr/local/bin/host_runner"),
        QStringLiteral("/usr/bin/host_runner"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    QProcess which;
    which.start(
        QStringLiteral("flatpak-spawn"),
        {QStringLiteral("--host"), QStringLiteral("which"), QStringLiteral("host_runner")});
    if (which.waitForFinished(2000) && which.exitCode() == 0) {
        const auto path = QString::fromLocal8Bit(which.readAllStandardOutput()).trimmed();
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            return path;
        }
    }
    return {};
}

} // namespace archstreamer::gui
