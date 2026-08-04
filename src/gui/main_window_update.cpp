#include "main_window.hpp"

#include "gui_logging.hpp"
#ifdef ARCHSTREAMER_HAS_HOST
#include "gui_host_runner.hpp"
#endif

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <filesystem>

namespace archstreamer::gui {
namespace {

QString find_python_executable() {
#ifdef Q_OS_WIN
    const QStringList candidates = {
        QStringLiteral("py"),
        QStringLiteral("python"),
        QStringLiteral("python3"),
    };
#else
    const QStringList candidates = {
        QStringLiteral("python3"),
        QStringLiteral("python"),
    };
#endif
    for (const auto& name : candidates) {
        const auto path = QStandardPaths::findExecutable(name);
        if (!path.isEmpty()) {
            return path;
        }
    }
    return {};
}

std::filesystem::path detect_repo_root() {
    const QStringList seeds = {
        QCoreApplication::applicationDirPath(),
        QFileInfo(QCoreApplication::applicationFilePath()).absolutePath(),
        QDir::currentPath(),
    };
    for (const auto& seed_q : seeds) {
        auto cur = std::filesystem::path(seed_q.toStdString());
        std::error_code ec;
        cur = std::filesystem::weakly_canonical(cur, ec);
        if (ec) {
            cur = std::filesystem::path(seed_q.toStdString());
        }
        for (int i = 0; i < 8; ++i) {
            if (std::filesystem::is_regular_file(cur / "CMakeLists.txt") &&
                std::filesystem::is_directory(cur / ".git")) {
                return cur;
            }
            if (!cur.has_parent_path() || cur == cur.parent_path()) {
                break;
            }
            cur = cur.parent_path();
        }
    }
    return {};
}

QString parse_check_field(const QString& output, const QString& key) {
    const QString prefix = key + QLatin1Char('=');
    const auto lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        const auto trimmed = line.trimmed();
        if (trimmed.startsWith(prefix)) {
            return trimmed.mid(prefix.size()).trimmed();
        }
    }
    return {};
}

} // namespace

QWidget* MainWindow::build_updates_group(QWidget* parent) {
    auto* box = new QGroupBox("Updates", parent);
    auto* form = new QFormLayout(box);

#ifdef ARCHSTREAMER_HAS_HOST
    if (running_inside_flatpak()) {
        auto* note = new QLabel(
            "Self-update is disabled inside Flatpak. Update the Flatpak package instead.",
            box);
        note->setWordWrap(true);
        form->addRow(note);
        return box;
    }
#endif

    settings_update_repo_ = new QLineEdit(box);
    settings_update_repo_->setPlaceholderText(
        QStringLiteral("auto-detect from this binary / current directory"));
    settings_update_repo_->setToolTip(
        "Git checkout that contains CMakeLists.txt and .git.\n"
        "On Windows this is usually Documents\\RetroStreamer.\n"
        "On Linux this is usually the repo with build/archstreamer_gui.");

    settings_update_branch_ = new QComboBox(box);
    settings_update_branch_->setEditable(true);
    settings_update_branch_->addItems({QStringLiteral("master"), QStringLiteral("dev")});
    settings_update_branch_->setCurrentText(QStringLiteral("master"));
    settings_update_branch_->setToolTip(
        "Branch to track for this session only (not saved).\n"
        "Default is master. Launch with --branch <name> to override\n"
        "(e.g. archstreamer_gui --branch dev).");

    settings_update_status_ = new QLabel(
        "Click Check for updates to compare this checkout with origin.",
        box);
    settings_update_status_->setWordWrap(true);

    auto* buttons = new QWidget(box);
    auto* row = new QHBoxLayout(buttons);
    row->setContentsMargins(0, 0, 0, 0);
    settings_update_check_ = new QPushButton("Check for updates", buttons);
    settings_update_apply_ = new QPushButton("Update now", buttons);
    settings_update_apply_->setEnabled(false);
    row->addWidget(settings_update_check_);
    row->addWidget(settings_update_apply_);
    row->addStretch(1);

    form->addRow("Repo", settings_update_repo_);
    form->addRow("Branch", settings_update_branch_);
    form->addRow(settings_update_status_);
    form->addRow(buttons);

    connect(settings_update_repo_, &QLineEdit::editingFinished, this, [this] {
        persist_settings_if_idle();
    });
    connect(settings_update_check_, &QPushButton::clicked, this, [this] {
        check_for_updates();
    });
    connect(settings_update_apply_, &QPushButton::clicked, this, [this] {
        apply_updates();
    });

    if (!session_update_branch_.isEmpty()) {
        apply_session_update_branch_to_ui();
    }

    return box;
}

void MainWindow::set_session_update_branch(const QString& branch) {
    const auto trimmed = branch.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    session_update_branch_ = trimmed;
    apply_session_update_branch_to_ui();
}

void MainWindow::apply_session_update_branch_to_ui() {
    if (settings_update_branch_ == nullptr || session_update_branch_.isEmpty()) {
        return;
    }
    const auto index = settings_update_branch_->findText(session_update_branch_);
    if (index >= 0) {
        settings_update_branch_->setCurrentIndex(index);
    } else {
        settings_update_branch_->setEditText(session_update_branch_);
    }
    set_update_status(
        QStringLiteral("Tracking origin/%1 this session (--branch).")
            .arg(session_update_branch_));
}

QString MainWindow::update_repo_path() const {
    if (settings_update_repo_ != nullptr) {
        const auto typed = settings_update_repo_->text().trimmed();
        if (!typed.isEmpty()) {
            return typed;
        }
    }
    const auto detected = detect_repo_root();
    if (!detected.empty()) {
        return QString::fromStdString(detected.string());
    }
    return {};
}

QString MainWindow::update_branch_name() const {
    if (settings_update_branch_ == nullptr) {
        return QStringLiteral("master");
    }
    const auto branch = settings_update_branch_->currentText().trimmed();
    return branch.isEmpty() ? QStringLiteral("master") : branch;
}

QString MainWindow::self_update_script_path(const QString& repo) const {
    const QDir root(repo);
    const auto script = root.absoluteFilePath(QStringLiteral("deploy/gui_self_update.py"));
    if (QFileInfo::exists(script)) {
        return script;
    }
    return {};
}

void MainWindow::check_for_updates() {
    if (update_busy_) {
        return;
    }
#ifdef ARCHSTREAMER_HAS_HOST
    if (running_inside_flatpak()) {
        set_update_status(QStringLiteral("Self-update is disabled inside Flatpak."));
        return;
    }
#endif

    const auto repo = update_repo_path();
    if (repo.isEmpty()) {
        set_update_status(
            QStringLiteral("Could not find the git repo. Set the Repo path under Updates."));
        return;
    }
    if (settings_update_repo_ != nullptr && settings_update_repo_->text().trimmed().isEmpty()) {
        settings_update_repo_->setText(repo);
    }

    const auto script = self_update_script_path(repo);
    if (script.isEmpty()) {
        set_update_status(
            QStringLiteral("deploy/gui_self_update.py missing in %1").arg(repo));
        return;
    }
    const auto python = find_python_executable();
    if (python.isEmpty()) {
        set_update_status(QStringLiteral("Python not found on PATH (need python3/python/py)."));
        return;
    }

    update_busy_ = true;
    if (settings_update_check_ != nullptr) {
        settings_update_check_->setEnabled(false);
    }
    if (settings_update_apply_ != nullptr) {
        settings_update_apply_->setEnabled(false);
    }
    set_update_status(QStringLiteral("Checking origin/%1 …").arg(update_branch_name()));

    auto* process = new QProcess(this);
    process->setProgram(python);
    QStringList args;
#ifdef Q_OS_WIN
    if (QFileInfo(python).fileName().compare(QStringLiteral("py"), Qt::CaseInsensitive) == 0) {
        args << QStringLiteral("-3");
    }
#endif
    args << script
         << QStringLiteral("check")
         << QStringLiteral("--repo") << repo
         << QStringLiteral("--branch") << update_branch_name();
    process->setArguments(args);
    process->setWorkingDirectory(repo);
    process->setProcessChannelMode(QProcess::MergedChannels);

    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus) {
        const auto output = QString::fromLocal8Bit(process->readAllStandardOutput());
        process->deleteLater();
        update_busy_ = false;
        if (settings_update_check_ != nullptr) {
            settings_update_check_->setEnabled(true);
        }

        if (settings_log_ != nullptr && !output.trimmed().isEmpty()) {
            append_log(settings_log_, QStringLiteral("[update]\n%1").arg(output.trimmed()));
        }

        const auto status = parse_check_field(output, QStringLiteral("status"));
        const auto behind = parse_check_field(output, QStringLiteral("behind"));
        const auto local = parse_check_field(output, QStringLiteral("local"));
        const auto remote = parse_check_field(output, QStringLiteral("remote"));
        const auto subject = parse_check_field(output, QStringLiteral("subject"));

        if (code != 0 && status.isEmpty()) {
            set_update_status(
                QStringLiteral("Update check failed (exit %1). See Settings log.").arg(code));
            if (settings_update_apply_ != nullptr) {
                settings_update_apply_->setEnabled(false);
            }
            return;
        }

        if (status == QLatin1String("up_to_date")) {
            set_update_status(
                QStringLiteral("Up to date (%1)%2")
                    .arg(local.isEmpty() ? QStringLiteral("?") : local)
                    .arg(subject.isEmpty() ? QString() : QStringLiteral(" — %1").arg(subject)));
            if (settings_update_apply_ != nullptr) {
                settings_update_apply_->setEnabled(false);
            }
            return;
        }

        if (status == QLatin1String("update_available")) {
            set_update_status(
                QStringLiteral("Update available: %1 commit(s) behind (local %2 → remote %3).")
                    .arg(behind.isEmpty() ? QStringLiteral("?") : behind)
                    .arg(local.isEmpty() ? QStringLiteral("?") : local)
                    .arg(remote.isEmpty() ? QStringLiteral("?") : remote));
            if (settings_update_apply_ != nullptr) {
                settings_update_apply_->setEnabled(true);
            }
            return;
        }

        if (status == QLatin1String("ahead_of_remote")) {
            set_update_status(
                QStringLiteral("Local checkout is ahead of origin/%1 (local %2).")
                    .arg(update_branch_name())
                    .arg(local.isEmpty() ? QStringLiteral("?") : local));
            if (settings_update_apply_ != nullptr) {
                // Still allow force sync via Update now (reset-hard).
                settings_update_apply_->setEnabled(true);
            }
            return;
        }

        set_update_status(
            QStringLiteral("Check result: %1 (see Settings log for details).")
                .arg(status.isEmpty() ? QStringLiteral("unknown") : status));
        if (settings_update_apply_ != nullptr) {
            settings_update_apply_->setEnabled(status == QLatin1String("update_available"));
        }
    });

    process->start();
    if (!process->waitForStarted(5'000)) {
        update_busy_ = false;
        if (settings_update_check_ != nullptr) {
            settings_update_check_->setEnabled(true);
        }
        set_update_status(QStringLiteral("Failed to start Python update checker."));
        process->deleteLater();
    }
}

void MainWindow::apply_updates() {
    if (update_busy_) {
        return;
    }
#ifdef ARCHSTREAMER_HAS_HOST
    if (running_inside_flatpak()) {
        set_update_status(QStringLiteral("Self-update is disabled inside Flatpak."));
        return;
    }
#endif

    const auto repo = update_repo_path();
    const auto script = self_update_script_path(repo);
    const auto python = find_python_executable();
    if (repo.isEmpty() || script.isEmpty() || python.isEmpty()) {
        set_update_status(QStringLiteral("Update cannot start (repo/script/python missing)."));
        return;
    }

    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("Update ArchStreamer"),
        QStringLiteral(
            "This will:\n"
            "• reset the repo to origin/%1 (discards local edits in that checkout)\n"
            "• rebuild ArchStreamer\n"
#ifdef Q_OS_WIN
            "• reinstall into Program Files (may need Admin)\n"
#endif
            "• quit this app and relaunch when finished\n\n"
            "Continue?")
            .arg(update_branch_name()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    persist_settings_if_idle();
    set_update_status(QStringLiteral("Starting update — this window will close…"));

    QStringList args;
#ifdef Q_OS_WIN
    if (QFileInfo(python).fileName().compare(QStringLiteral("py"), Qt::CaseInsensitive) == 0) {
        args << QStringLiteral("-3");
    }
#endif
    args << script
         << QStringLiteral("apply")
         << QStringLiteral("--repo") << repo
         << QStringLiteral("--branch") << update_branch_name()
         << QStringLiteral("--reset-hard")
         << QStringLiteral("--launch")
         << QStringLiteral("--wait-secs") << QStringLiteral("2");
#ifdef ARCHSTREAMER_HAS_HOST
    args << QStringLiteral("--build-host");
#endif

    const bool started = QProcess::startDetached(python, args, repo);
    if (!started) {
        set_update_status(QStringLiteral("Failed to start the update process."));
        return;
    }

    // Quit so binaries can be overwritten (especially on Windows).
    QCoreApplication::quit();
}

void MainWindow::set_update_status(const QString& text) {
    if (settings_update_status_ != nullptr) {
        settings_update_status_->setText(text);
    }
    if (settings_log_ != nullptr) {
        append_log(settings_log_, QStringLiteral("[update] %1").arg(text));
    }
}

} // namespace archstreamer::gui
