#include "main_window.hpp"

#include "gui_logging.hpp"
#include "gui_util.hpp"
#include "remote_ssh.hpp"

#include "common/remote_host.hpp"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <chrono>
#include <thread>

namespace archstreamer::gui {
namespace {

bool try_probe_session(
    ClientApp& app,
    const std::string& host,
    std::uint16_t control_port,
    ActiveSessionInfo* out_info,
    QString* out_error) {
    try {
        *out_info = app.active_session_info(host, control_port);
        return true;
    } catch (const std::exception& error) {
        if (out_error != nullptr) {
            *out_error = QString::fromStdString(error.what());
        }
        return false;
    }
}

bool lobby_usable_for_request(
    const ActiveSessionInfo& info,
    const std::string& want_resolved_id,
    const std::string& process_gpu_arg,
    const std::vector<std::pair<std::string, std::string>>& gpu_options) {
    if (remote_host_lobby_full(info.active_slots, info.max_slots)) {
        return false;
    }
    if (want_resolved_id.empty()) {
        return true;
    }
    // No --gpu / auto on the process → host default = first --list-gpus entry (score order).
    if (process_gpu_arg.empty() || process_gpu_arg == "auto") {
        return !gpu_options.empty() && gpu_options.front().first == want_resolved_id;
    }
    const auto process_match = remote_host_match_gpu_option(gpu_options, process_gpu_arg);
    if (!process_match.has_value()) {
        return false;
    }
    return process_match->first == want_resolved_id;
}

} // namespace

QWidget* MainWindow::build_remote_tab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    auto* form_box = new QGroupBox("Remote host (SSH)", page);
    auto* form = new QFormLayout(form_box);

    remote_ssh_host_ = new QLineEdit(form_box);
    remote_ssh_host_->setPlaceholderText("IP or hostname of the machine that runs host_runner");
    remote_ssh_user_ = new QLineEdit(form_box);
    remote_ssh_password_ = new QLineEdit(form_box);
    remote_ssh_password_->setEchoMode(QLineEdit::Password);
    remote_ssh_password_->setPlaceholderText("session only — not saved");
    remote_ssh_password_->setToolTip(
        "SSH password for the remote machine. Not persisted; re-enter when you open the app.");
    remote_ssh_port_ = new QSpinBox(form_box);
    remote_ssh_port_->setRange(1, 65535);
    remote_ssh_port_->setValue(22);
    remote_directory_ = new QLineEdit(form_box);
    remote_directory_->setPlaceholderText("e.g. /home/user/ArchStreamer/build");
    remote_rom_root_ = new QLineEdit(form_box);
    remote_rom_root_->setPlaceholderText("absolute ROM root on the remote machine");
    remote_binary_ = new QLineEdit(form_box);
    remote_binary_->setText(QStringLiteral("./host_runner"));
    remote_binary_->setPlaceholderText(QStringLiteral("./host_runner or …/build/host_runner"));
    remote_binary_->setToolTip(
        "Path to host_runner on the remote machine (Path A, or GPU listing).\n"
        "If you paste a build directory, /host_runner is appended automatically.\n"
        "When a start script is set (Path B), this is only used for --list-gpus.");
    remote_start_script_ = new QLineEdit(form_box);
    remote_start_script_->setPlaceholderText(
        QStringLiteral("optional — e.g. /home/alina/bin/archstreamer-start"));
    remote_start_script_->setToolTip(
        "Optional remote start script (Path B).\n"
        "Empty: Ensure Host starts host_runner with full args (Path A).\n"
        "Set: Ensure Host runs this script with ports + GPU only; the script\n"
        "owns ROM root / host_runner path / setup. Absolute path preferred.");
    remote_base_control_port_ = new QSpinBox(form_box);
    remote_base_control_port_->setRange(1, 65535);
    remote_base_control_port_->setValue(static_cast<int>(RemoteDefaultControlPort));
    remote_base_input_port_ = new QSpinBox(form_box);
    remote_base_input_port_->setRange(1, 65535);
    remote_base_input_port_->setValue(static_cast<int>(RemoteDefaultInputPort));
    remote_gpu_ = new QLineEdit(form_box);
    remote_gpu_->setPlaceholderText("optional — e.g. 3060, amd, nvidia:1");
    remote_gpu_->setToolTip(
        "Optional GPU for Ensure Host (not broadcast on the control protocol).\n"
        "Empty: use the host default (auto) and reuse any free lobby.\n"
        "Set: SSH-lists GPUs via host_runner --list-gpus (same list as Settings),\n"
        "fuzzy-matches your text (3060, amd, …), reuses a free lobby on that GPU,\n"
        "or starts a new instance with --gpu <resolved-id>.");

    form->addRow("SSH host", remote_ssh_host_);
    form->addRow("SSH user", remote_ssh_user_);
    form->addRow("SSH password", remote_ssh_password_);
    form->addRow("SSH port", remote_ssh_port_);
    form->addRow("Remote directory", remote_directory_);
    form->addRow("Remote ROM root", remote_rom_root_);
    form->addRow("host_runner path", remote_binary_);
    form->addRow("Start script (optional)", remote_start_script_);
    form->addRow("Base control port", remote_base_control_port_);
    form->addRow("Base input port", remote_base_input_port_);
    form->addRow("GPU (optional)", remote_gpu_);

    auto* buttons = new QHBoxLayout();
    auto* ensure = new QPushButton("Ensure Host", page);
    auto* stop = new QPushButton("Stop Host", page);
    buttons->addWidget(ensure);
    buttons->addWidget(stop);
    buttons->addStretch(1);

    remote_status_ = new QLabel(
        "Ensure Host probes port blocks, reuses a free matching lobby, or SSH-starts "
        "host_runner (or an optional start script with ports + GPU).",
        page);
    remote_status_->setWordWrap(true);
    remote_log_ = new QPlainTextEdit(page);
    remote_log_->setObjectName("remoteLog");
    remote_log_->setReadOnly(true);
    remote_log_->setMaximumBlockCount(500);

    connect(ensure, &QPushButton::clicked, this, [this] { ensure_remote_host(); });
    connect(stop, &QPushButton::clicked, this, [this] { stop_remote_host(); });
    for (auto* edit :
         {remote_ssh_host_,
          remote_ssh_user_,
          remote_directory_,
          remote_rom_root_,
          remote_binary_,
          remote_start_script_,
          remote_gpu_}) {
        connect(edit, &QLineEdit::editingFinished, this, [this] { persist_settings_if_idle(); });
    }
    connect(remote_ssh_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(remote_base_control_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        persist_settings_if_idle();
    });
    connect(remote_base_input_port_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        persist_settings_if_idle();
    });

    root->addWidget(form_box);
    root->addLayout(buttons);
    root->addWidget(remote_status_);
    root->addWidget(remote_log_, 1);
    return page;
}

void MainWindow::set_remote_status(const QString& text) {
    if (remote_status_ != nullptr) {
        remote_status_->setText(text);
    }
    append_log(remote_log_, text);
}

void MainWindow::ensure_remote_host() {
    if (remote_busy_) {
        set_remote_status(QStringLiteral("Remote action already running."));
        return;
    }
    const auto ssh_host = remote_ssh_host_->text().trimmed();
    const auto ssh_user = remote_ssh_user_->text().trimmed();
    const auto password = remote_ssh_password_->text();
    const auto directory = remote_directory_->text().trimmed();
    const auto rom_root = remote_rom_root_->text().trimmed();
    const auto binary = remote_binary_->text().trimmed().isEmpty()
        ? QStringLiteral("./host_runner")
        : remote_binary_->text().trimmed();
    const auto start_script =
        remote_start_script_ != nullptr ? remote_start_script_->text().trimmed() : QString();
    const auto want_gpu = remote_gpu_ != nullptr ? remote_gpu_->text().trimmed() : QString();
    const int ssh_port = remote_ssh_port_->value();
    const auto base_control =
        static_cast<std::uint16_t>(remote_base_control_port_->value());
    const auto base_input =
        static_cast<std::uint16_t>(remote_base_input_port_->value());

    if (ssh_host.isEmpty() || ssh_user.isEmpty() || directory.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote host"),
            QStringLiteral("SSH host, user, and remote directory are required."));
        return;
    }
    if (start_script.isEmpty() && rom_root.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote host"),
            QStringLiteral(
                "Remote ROM root is required unless a start script is set "
                "(Path B: the script owns ROM root / host_runner)."));
        return;
    }
    if (password.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote host"),
            QStringLiteral("Enter the SSH password (it is not saved)."));
        return;
    }

    persist_settings_if_idle();

    remote_busy_ = true;
    if (want_gpu.isEmpty()) {
        set_remote_status(QStringLiteral("Probing %1:%2…").arg(ssh_host).arg(base_control));
    } else {
        set_remote_status(
            QStringLiteral("Probing %1 for a free lobby on GPU “%2”…")
                .arg(ssh_host)
                .arg(want_gpu));
    }

    std::thread([this,
                 ssh_host,
                 ssh_user,
                 password,
                 directory,
                 rom_root,
                 binary,
                 start_script,
                 want_gpu,
                 ssh_port,
                 base_control,
                 base_input] {
        auto finish = [this](const QString& status, bool apply, const QString& host,
                             int control, int input, int tracked_control) {
            QMetaObject::invokeMethod(
                this,
                [this, status, apply, host, control, input, tracked_control] {
                    remote_busy_ = false;
                    set_remote_status(status);
                    if (apply) {
                        apply_client_host(host, control, input, QStringLiteral("Remote"));
                        remote_tracked_control_port_ = tracked_control;
                    }
                    persist_settings_if_idle();
                },
                Qt::QueuedConnection);
        };

        const auto host_std = ssh_host.toStdString();
        std::vector<std::pair<std::string, std::string>> gpu_options;
        std::string resolved_gpu_id;
        QString resolved_gpu_label;

        if (!want_gpu.isEmpty()) {
            const auto list_cmd = QString::fromStdString(remote_host_list_gpus_shell(
                directory.toStdString(), binary.toStdString()));
            QMetaObject::invokeMethod(
                this,
                [this] {
                    set_remote_status(QStringLiteral("Listing remote GPUs (host_runner --list-gpus)…"));
                },
                Qt::QueuedConnection);
            const auto listed = run_remote_ssh_command(
                ssh_host, ssh_port, ssh_user, password, list_cmd);
            if (!listed.ok) {
                finish(
                    QStringLiteral("Remote GPU list failed: %1%2")
                        .arg(listed.error)
                        .arg(listed.stderr_text.isEmpty()
                            ? QString()
                            : QStringLiteral("\n%1").arg(listed.stderr_text)),
                    false,
                    {},
                    0,
                    0,
                    0);
                return;
            }
            const auto lines = listed.stdout_text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const auto& line : lines) {
                const auto tab = line.indexOf(QLatin1Char('\t'));
                if (tab <= 0) {
                    continue;
                }
                gpu_options.emplace_back(
                    line.left(tab).trimmed().toStdString(),
                    line.mid(tab + 1).trimmed().toStdString());
            }
            const auto matched =
                remote_host_match_gpu_option(gpu_options, want_gpu.toStdString());
            if (!matched.has_value()) {
                QString available;
                for (const auto& [id, name] : gpu_options) {
                    if (!available.isEmpty()) {
                        available += QStringLiteral(", ");
                    }
                    available += QString::fromStdString(name + " [" + id + "]");
                }
                finish(
                    QStringLiteral("No remote GPU matched “%1”. Available: %2")
                        .arg(want_gpu)
                        .arg(available.isEmpty() ? QStringLiteral("(none)") : available),
                    false,
                    {},
                    0,
                    0,
                    0);
                return;
            }
            resolved_gpu_id = matched->first;
            resolved_gpu_label = QStringLiteral("%1 [%2]")
                .arg(QString::fromStdString(matched->second))
                .arg(QString::fromStdString(matched->first));
            QMetaObject::invokeMethod(
                this,
                [this, resolved_gpu_label] {
                    set_remote_status(
                        QStringLiteral("Matched remote GPU %1 — probing lobbies…")
                            .arg(resolved_gpu_label));
                },
                Qt::QueuedConnection);
        }

        auto query_process_gpu = [&](std::uint16_t control_port) -> std::string {
            if (resolved_gpu_id.empty()) {
                return {};
            }
            const auto cmd = QString::fromStdString(
                remote_host_encode_gpu_query_shell(control_port));
            const auto ssh = run_remote_ssh_command(
                ssh_host, ssh_port, ssh_user, password, cmd, 15'000);
            if (!ssh.ok) {
                return {};
            }
            return ssh.stdout_text.trimmed().toStdString();
        };

        auto start_instance = [&](int instance_index, const RemoteHostPortBlock& ports) -> bool {
            const auto cmd = QString::fromStdString(remote_host_start_shell(
                directory.toStdString(),
                binary.toStdString(),
                rom_root.toStdString(),
                ports,
                resolved_gpu_id,
                start_script.toStdString()));
            QMetaObject::invokeMethod(
                this,
                [this, instance_index, ports, resolved_gpu_label, start_script, cmd] {
                    QString msg = start_script.isEmpty()
                        ? QStringLiteral("SSH-starting host instance %1 on port %2")
                              .arg(instance_index)
                              .arg(ports.control_port)
                        : QStringLiteral("SSH-starting via script (instance %1, port %2)")
                              .arg(instance_index)
                              .arg(ports.control_port);
                    if (!resolved_gpu_label.isEmpty()) {
                        msg += QStringLiteral(" (%1)").arg(resolved_gpu_label);
                    }
                    msg += QStringLiteral("…");
                    set_remote_status(msg);
                    append_log(remote_log_, QStringLiteral("ssh cmd: %1").arg(cmd));
                },
                Qt::QueuedConnection);
            const auto ssh = run_remote_ssh_command(
                ssh_host, ssh_port, ssh_user, password, cmd);
            if (!ssh.ok) {
                finish(
                    QStringLiteral("SSH start failed: %1%2")
                        .arg(ssh.error)
                        .arg(ssh.stderr_text.isEmpty()
                            ? QString()
                            : QStringLiteral("\n%1").arg(ssh.stderr_text)),
                    false,
                    {},
                    0,
                    0,
                    0);
                return false;
            }
            for (int attempt = 0; attempt < 20; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                ActiveSessionInfo started{};
                QString started_error;
                if (try_probe_session(
                        client_app_, host_std, ports.control_port, &started, &started_error)) {
                    QString gpu_note;
                    if (!resolved_gpu_label.isEmpty()) {
                        gpu_note = QStringLiteral(" %1").arg(resolved_gpu_label);
                    }
                    finish(
                        QStringLiteral("Started host on %1:%2%3")
                            .arg(ssh_host)
                            .arg(ports.control_port)
                            .arg(gpu_note),
                        true,
                        ssh_host,
                        ports.control_port,
                        ports.input_port,
                        ports.control_port);
                    return true;
                }
            }
            finish(
                QStringLiteral(
                    "SSH start reported success but control port %1 never answered")
                    .arg(ports.control_port),
                false,
                {},
                0,
                0,
                0);
            return false;
        };

        for (int n = 0; n <= 8; ++n) {
            const auto ports = remote_host_port_block(n, base_control, base_input);
            ActiveSessionInfo info{};
            QString probe_error;
            if (try_probe_session(client_app_, host_std, ports.control_port, &info, &probe_error)) {
                const auto process_gpu = query_process_gpu(ports.control_port);
                if (lobby_usable_for_request(info, resolved_gpu_id, process_gpu, gpu_options)) {
                    const QString slot_text = (info.active_slots && info.max_slots)
                        ? QStringLiteral(" (slots %1/%2)")
                            .arg(*info.active_slots)
                            .arg(*info.max_slots)
                        : QString();
                    QString gpu_text;
                    if (!resolved_gpu_label.isEmpty()) {
                        gpu_text = QStringLiteral(" %1").arg(resolved_gpu_label);
                    }
                    finish(
                        QStringLiteral("Reusing host on %1:%2%3%4")
                            .arg(ssh_host)
                            .arg(ports.control_port)
                            .arg(slot_text)
                            .arg(gpu_text),
                        true,
                        ssh_host,
                        ports.control_port,
                        ports.input_port,
                        ports.control_port);
                    return;
                }
                continue;
            }
            if (n == 0) {
                QMetaObject::invokeMethod(
                    this,
                    [this, probe_error] {
                        set_remote_status(
                            QStringLiteral("No host on base port (%1) — will SSH-start…")
                                .arg(probe_error));
                    },
                    Qt::QueuedConnection);
            }
            start_instance(n, ports);
            return;
        }

        finish(
            want_gpu.isEmpty()
                ? QStringLiteral("All probed host instances are full.")
                : QStringLiteral(
                      "No free lobby on GPU “%1” (existing instances full or different GPU).")
                      .arg(resolved_gpu_label.isEmpty() ? want_gpu : resolved_gpu_label),
            false,
            {},
            0,
            0,
            0);
    }).detach();
}

void MainWindow::stop_remote_host() {
    if (remote_busy_) {
        set_remote_status(QStringLiteral("Remote action already running."));
        return;
    }
    const auto ssh_host = remote_ssh_host_->text().trimmed();
    const auto ssh_user = remote_ssh_user_->text().trimmed();
    const auto password = remote_ssh_password_->text();
    const auto directory = remote_directory_->text().trimmed();
    const int ssh_port = remote_ssh_port_->value();
    const int control = remote_tracked_control_port_ > 0
        ? remote_tracked_control_port_
        : (remote_base_control_port_ != nullptr ? remote_base_control_port_->value() : 45555);

    if (ssh_host.isEmpty() || ssh_user.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote host"),
            QStringLiteral("SSH host and user are required to stop."));
        return;
    }
    if (password.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote host"),
            QStringLiteral("Enter the SSH password (it is not saved)."));
        return;
    }

    persist_settings_if_idle();
    remote_busy_ = true;
    set_remote_status(QStringLiteral("Stopping remote host on control port %1…").arg(control));

    std::thread([this, ssh_host, ssh_user, password, directory, ssh_port, control] {
        const auto cmd = QString::fromStdString(remote_host_stop_shell(
            static_cast<std::uint16_t>(control),
            directory.toStdString()));
        const auto ssh = run_remote_ssh_command(
            ssh_host, ssh_port, ssh_user, password, cmd);
        QMetaObject::invokeMethod(
            this,
            [this, ssh, control] {
                remote_busy_ = false;
                if (!ssh.ok) {
                    set_remote_status(
                        QStringLiteral("Stop failed: %1%2")
                            .arg(ssh.error)
                            .arg(ssh.stderr_text.isEmpty()
                                ? QString()
                                : QStringLiteral("\n%1").arg(ssh.stderr_text)));
                } else {
                    set_remote_status(
                        QStringLiteral("Stopped remote host on control port %1.").arg(control));
                    remote_tracked_control_port_ = 0;
                }
                persist_settings_if_idle();
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace archstreamer::gui
