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

bool path_exists(const QString& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path.toStdString()), ec);
}

bool is_git_checkout_root(const QString& path) {
    const QDir dir(path);
    if (!dir.exists(QStringLiteral("CMakeLists.txt"))) {
        return false;
    }
    // Plain repos use a .git directory; worktrees/submodules use a .git file.
    return path_exists(dir.absoluteFilePath(QStringLiteral(".git")));
}

bool has_self_update_script(const QString& repo) {
    return QFileInfo::exists(
        QDir(repo).absoluteFilePath(QStringLiteral("deploy/gui_self_update.py")));
}

QString strip_path_quotes(QString path) {
    path = path.trimmed();
    if (path.size() >= 2) {
        const QChar a = path.front();
        const QChar b = path.back();
        if ((a == QLatin1Char('"') && b == QLatin1Char('"')) ||
            (a == QLatin1Char('\'') && b == QLatin1Char('\''))) {
            path = path.mid(1, path.size() - 2).trimmed();
        }
    }
    return path;
}

/** Accept repo root, deploy/, or a file under the checkout; walk up to CMakeLists+.git. */
QString resolve_repo_checkout(QString typed) {
    typed = strip_path_quotes(std::move(typed));
    if (typed.isEmpty()) {
        return {};
    }

    QFileInfo info(typed);
    QString start = info.exists()
        ? (info.isFile() ? info.absolutePath() : info.absoluteFilePath())
        : QDir::cleanPath(typed);

    QDir dir(start);
    for (int i = 0; i < 8; ++i) {
        const QString candidate = dir.absolutePath();
        if (is_git_checkout_root(candidate)) {
            return QDir::toNativeSeparators(candidate);
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QDir::toNativeSeparators(start);
}

QString explain_repo_problem(const QString& repo) {
    if (repo.isEmpty()) {
        return QStringLiteral(
            "Could not find the git repo. Set Repo under Updates to a shared checkout "
            "(Program Files installs cannot auto-detect).");
    }
    if (!path_exists(repo)) {
        return QStringLiteral(
            "Repo path not found (or this Windows account cannot access it):\n%1\n"
            "Put the checkout somewhere both users can read "
            "(e.g. C:\\dev\\ArchStreamer), not under another user's Documents.")
            .arg(repo);
    }
    if (!is_git_checkout_root(repo)) {
        return QStringLiteral(
            "Not an ArchStreamer git checkout (need CMakeLists.txt and .git):\n%1\n"
            "Point Repo at the repo root, not Program Files and not only the deploy\\ folder.")
            .arg(repo);
    }
    if (!has_self_update_script(repo)) {
        return QStringLiteral("deploy/gui_self_update.py missing in %1").arg(repo);
    }
    return {};
}

std::filesystem::path detect_repo_root() {
    QStringList seeds = {
        QCoreApplication::applicationDirPath(),
        QFileInfo(QCoreApplication::applicationFilePath()).absolutePath(),
        QDir::currentPath(),
    };
#ifdef Q_OS_WIN
    // Shared locations for multi-user PCs (Program Files install cannot walk to .git).
    seeds << QStringLiteral("C:/dev/ArchStreamer")
          << QStringLiteral("C:/dev/RetroStreamer")
          << QStringLiteral("C:/ArchStreamer")
          << QStringLiteral("C:/RetroStreamer")
          << QStringLiteral("C:/Users/Public/Documents/ArchStreamer")
          << QStringLiteral("C:/Users/Public/Documents/RetroStreamer");
#endif

    for (const auto& seed_q : seeds) {
        if (seed_q.isEmpty()) {
            continue;
        }
        const QString resolved = resolve_repo_checkout(seed_q);
        if (!resolved.isEmpty() && is_git_checkout_root(resolved) &&
            has_self_update_script(resolved)) {
            return std::filesystem::path(resolved.toStdString());
        }
        // Fall back: walk parents of the seed even without the script (older checkouts).
        auto cur = std::filesystem::path(seed_q.toStdString());
        std::error_code ec;
        cur = std::filesystem::weakly_canonical(cur, ec);
        if (ec) {
            cur = std::filesystem::path(seed_q.toStdString());
        }
        for (int i = 0; i < 8; ++i) {
            const QString candidate = QString::fromStdString(cur.string());
            if (is_git_checkout_root(candidate)) {
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

QString git_current_branch(const QString& repo) {
    if (repo.isEmpty()) {
        return {};
    }
    QProcess process;
    process.setProgram(QStringLiteral("git"));
    process.setArguments(
        {QStringLiteral("-C"), repo, QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
         QStringLiteral("HEAD")});
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForFinished(5000) || process.exitCode() != 0) {
        return {};
    }
    return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
}

bool confirm_branch_if_diverged(
    QWidget* parent,
    const QString& repo,
    const QString& selected_branch) {
    const auto current = git_current_branch(repo);
    if (current.isEmpty() ||
        current == QLatin1String("HEAD") ||
        current.compare(selected_branch, Qt::CaseSensitive) == 0) {
        return true;
    }
    const auto reply = QMessageBox::question(
        parent,
        QStringLiteral("Different update branch"),
        QStringLiteral(
            "This checkout is currently on \"%1\", but Updates is set to \"%2\".\n\n"
            "Continue using origin/%2?")
            .arg(current, selected_branch),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    return reply == QMessageBox::Yes;
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
        "Git checkout that contains CMakeLists.txt, .git, and deploy/gui_self_update.py.\n"
        "A Program Files install cannot auto-detect — set this explicitly.\n"
        "Multi-user Windows: use a shared folder (e.g. C:\\dev\\ArchStreamer),\n"
        "not another account's Documents. You can paste the repo root or the deploy\\ folder.");

    settings_update_branch_ = new QComboBox(box);
    settings_update_branch_->setEditable(true);
    settings_update_branch_->addItems({QStringLiteral("master"), QStringLiteral("dev")});
    settings_update_branch_->setCurrentText(QStringLiteral("master"));
    settings_update_branch_->setToolTip(
        "Branch to pull/build for Updates.\n"
        "The last value you enter is remembered for next launch.\n"
        "Launch with --branch <name> to override for this session only\n"
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
    connect(settings_update_branch_, &QComboBox::currentTextChanged, this, [this](const QString&) {
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
            return resolve_repo_checkout(typed);
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
    if (repo.isEmpty() || !has_self_update_script(repo)) {
        return {};
    }
    return QDir(repo).absoluteFilePath(QStringLiteral("deploy/gui_self_update.py"));
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
    if (const auto problem = explain_repo_problem(repo); !problem.isEmpty()) {
        set_update_status(problem);
        return;
    }
    if (settings_update_repo_ != nullptr &&
        settings_update_repo_->text().trimmed().isEmpty()) {
        settings_update_repo_->setText(repo);
    }

    const auto branch = update_branch_name();
    if (!confirm_branch_if_diverged(this, repo, branch)) {
        set_update_status(
            QStringLiteral("Update check cancelled (branch still \"%1\").")
                .arg(git_current_branch(repo)));
        return;
    }

    const auto script = self_update_script_path(repo);
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
    set_update_status(QStringLiteral("Checking origin/%1 …").arg(branch));

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
         << QStringLiteral("--branch") << branch;
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
    if (const auto problem = explain_repo_problem(repo); !problem.isEmpty()) {
        set_update_status(problem);
        return;
    }
    const auto script = self_update_script_path(repo);
    const auto python = find_python_executable();
    if (python.isEmpty()) {
        set_update_status(QStringLiteral("Python not found on PATH (need python3/python/py)."));
        return;
    }

    const auto branch = update_branch_name();
    if (!confirm_branch_if_diverged(this, repo, branch)) {
        set_update_status(
            QStringLiteral("Update cancelled (branch still \"%1\").")
                .arg(git_current_branch(repo)));
        return;
    }

    // Keep each QStringLiteral contiguous: MSVC rejects #ifdef inside macro args
    // (QStringLiteral is a macro), which is the classic Windows build break here.
#ifdef Q_OS_WIN
    const QString prompt = QStringLiteral(
        "This will:\n"
        "- reset the repo to origin/%1 (discards local edits in that checkout)\n"
        "- rebuild ArchStreamer\n"
        "- reinstall into Program Files (may need Admin)\n"
        "- quit this app and relaunch when finished\n\n"
        "Continue?");
#else
    const QString prompt = QStringLiteral(
        "This will:\n"
        "- reset the repo to origin/%1 (discards local edits in that checkout)\n"
        "- rebuild ArchStreamer\n"
        "- quit this app and relaunch when finished\n\n"
        "Continue?");
#endif
    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("Update ArchStreamer"),
        prompt.arg(branch),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    persist_settings_if_idle();
    set_update_status(QStringLiteral("Starting update - this window will close..."));

    QStringList args;
#ifdef Q_OS_WIN
    if (QFileInfo(python).fileName().compare(QStringLiteral("py"), Qt::CaseInsensitive) == 0) {
        args << QStringLiteral("-3");
    }
#endif
    args << script
         << QStringLiteral("apply")
         << QStringLiteral("--repo") << repo
         << QStringLiteral("--branch") << branch
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
