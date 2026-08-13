#include "main_window.hpp"

#include "gui_logging.hpp"
#include "gui_util.hpp"
#include "remote_ssh.hpp"

#include "common/remote_host.hpp"

#include <QAbstractItemView>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <chrono>
#include <thread>

namespace archstreamer::gui {
namespace {

constexpr int kRemoteRoleKind = Qt::UserRole;
constexpr int kRemoteRoleClientId = Qt::UserRole + 1;
constexpr int kRemoteRoleSlot = Qt::UserRole + 2;

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
    const std::string& want_gpu,
    const std::string& process_gpu_arg) {
    if (remote_host_lobby_full(info.active_slots, info.max_slots)) {
        return false;
    }
    if (want_gpu.empty()) {
        return true;
    }
    return remote_host_gpu_preference_matches(want_gpu, process_gpu_arg);
}

QString startup_selected_gpu(const QString& output) {
    for (const auto& raw_line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const auto line = raw_line.trimmed();
        if (!line.contains(QStringLiteral("[archstreamer-startup]")) ||
            !line.contains(QStringLiteral("gpu-role=encode"))) {
            continue;
        }
        if (line.contains(QStringLiteral("gpu-match=none"))) {
            return {};
        }
        const auto marker = QStringLiteral(" selected=");
        const auto start = line.indexOf(marker);
        if (start < 0) {
            continue;
        }
        auto value = line.mid(start + marker.size());
        const auto end = value.indexOf(QLatin1Char(' '));
        if (end >= 0) {
            value = value.left(end);
        }
        return value.trimmed();
    }
    return {};
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
    remote_host_config_ = new QLineEdit(form_box);
    remote_host_config_->setPlaceholderText(
        QStringLiteral("optional — e.g. /home/user/archstreamer-host.conf"));
    remote_host_config_->setToolTip(
        "Optional config on the remote machine.\n"
        "This can be the remote GUI settings file; ports, display, GPU, and extra args override it.\n"
        "Blank uses the default from the Paths tab.");
    remote_binary_ = new QLineEdit(form_box);
    remote_binary_->setText(QStringLiteral("host_runner"));
    remote_binary_->setPlaceholderText(QStringLiteral("host_runner or /path/to/script"));
    remote_binary_->setToolTip(
        "Remote executable to start. This can be host_runner or a personal wrapper script.\n"
        "The Remote tab only appends optional overrides; the executable decides what is required.");
    remote_base_control_port_ = new QSpinBox(form_box);
    remote_base_control_port_->setRange(1, 65535);
    remote_base_control_port_->setValue(static_cast<int>(RemoteDefaultControlPort));
    remote_base_input_port_ = new QSpinBox(form_box);
    remote_base_input_port_->setRange(1, 65535);
    remote_base_input_port_->setValue(static_cast<int>(RemoteDefaultInputPort));
    remote_gpu_ = new QLineEdit(form_box);
    remote_gpu_->setPlaceholderText("optional — e.g. 3060, amd, nvidia:1");
    remote_gpu_->setToolTip(
        "Optional --gpu override passed to the remote executable.\n"
        "Blank leaves GPU choice to the remote config or script.");
    remote_extra_args_ = new QLineEdit(form_box);
    remote_extra_args_->setPlaceholderText(QStringLiteral("optional — e.g. --flag value -x y"));
    remote_extra_args_->setToolTip(
        "Optional raw arguments appended after ArchStreamer overrides.\n"
        "Intended for custom scripts that accept extra flags.");

    form->addRow("SSH host", remote_ssh_host_);
    form->addRow("SSH user", remote_ssh_user_);
    form->addRow("SSH password", remote_ssh_password_);
    form->addRow("SSH port", remote_ssh_port_);
    form->addRow("Host config (optional)", remote_host_config_);
    form->addRow("Executable path", remote_binary_);
    form->addRow("Base control port", remote_base_control_port_);
    form->addRow("Base input port", remote_base_input_port_);
    form->addRow("GPU (optional)", remote_gpu_);
    form->addRow("Args (optional)", remote_extra_args_);

    auto* buttons = new QHBoxLayout();
    auto* ensure = new QPushButton("Ensure Host", page);
    auto* stop = new QPushButton("Stop Host", page);
    buttons->addWidget(ensure);
    buttons->addWidget(stop);
    buttons->addStretch(1);

    remote_status_ = new QLabel(
        "Ensure Host probes port blocks, reuses a free matching lobby, or SSH-starts "
        "the configured remote executable with optional overrides. "
        "Remote users lists Connected/Active from cadence SQL on the host; Kick writes the "
        "same markers as the Users tab. Stop Host uses the tracked port, or the GPU field "
        "to find and stop that instance.",
        page);
    remote_status_->setWordWrap(true);

    auto* users_box = new QGroupBox("Remote users", page);
    auto* users_layout = new QVBoxLayout(users_box);
    remote_users_ = new QListWidget(users_box);
    remote_users_->setMinimumHeight(120);
    remote_users_->setMaximumHeight(200);
    remote_users_->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* users_buttons = new QHBoxLayout();
    remote_users_refresh_ = new QPushButton("Refresh users", users_box);
    remote_users_kick_ = new QPushButton("Kick selected", users_box);
    users_buttons->addWidget(remote_users_refresh_);
    users_buttons->addWidget(remote_users_kick_);
    users_buttons->addStretch(1);
    users_layout->addWidget(remote_users_);
    users_layout->addLayout(users_buttons);

    remote_log_ = new QPlainTextEdit(page);
    remote_log_->setObjectName("remoteLog");
    remote_log_->setReadOnly(true);
    remote_log_->setMaximumBlockCount(500);

    connect(ensure, &QPushButton::clicked, this, [this] { ensure_remote_host(); });
    connect(stop, &QPushButton::clicked, this, [this] { stop_remote_host(); });
    connect(remote_users_refresh_, &QPushButton::clicked, this, [this] { refresh_remote_users(); });
    connect(remote_users_kick_, &QPushButton::clicked, this, [this] { kick_remote_user(); });
    for (auto* edit :
         {remote_ssh_host_,
          remote_ssh_user_,
          remote_host_config_,
          remote_binary_,
          remote_gpu_,
          remote_extra_args_}) {
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
    root->addWidget(users_box);
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
    const auto host_config =
        remote_host_config_ != nullptr ? remote_host_config_->text().trimmed() : QString();
    const auto effective_host_config = host_config.isEmpty()
        ? QString::fromStdString(host_config_path().string())
        : host_config;
    const auto binary = remote_binary_->text().trimmed().isEmpty()
        ? QStringLiteral("host_runner")
        : remote_binary_->text().trimmed();
    const auto want_gpu = remote_gpu_ != nullptr ? remote_gpu_->text().trimmed() : QString();
    const auto extra_args =
        remote_extra_args_ != nullptr ? remote_extra_args_->text().trimmed() : QString();
    const int ssh_port = remote_ssh_port_->value();
    const auto base_control =
        static_cast<std::uint16_t>(remote_base_control_port_->value());
    const auto base_input =
        static_cast<std::uint16_t>(remote_base_input_port_->value());
    if (ssh_host.isEmpty() || ssh_user.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote host"),
            QStringLiteral("SSH host and user are required."));
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
                 effective_host_config,
                 binary,
                 want_gpu,
                 extra_args,
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
                        refresh_remote_users();
                    }
                    persist_settings_if_idle();
                },
                Qt::QueuedConnection);
        };

        const auto host_std = ssh_host.toStdString();
        const std::string resolved_gpu_id = want_gpu.toStdString();
        const QString resolved_gpu_label = want_gpu;

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
                binary.toStdString(),
                effective_host_config.toStdString(),
                ports,
                resolved_gpu_id,
                extra_args.toStdString()));
            QString command_summary = QStringLiteral(
                "ssh start: executable=%1 config=%2 ports=%3/%4/%5/%6 display=:%7 gpu=%8")
                .arg(binary)
                .arg(effective_host_config.isEmpty() ? QStringLiteral("(none)") : effective_host_config)
                .arg(ports.control_port)
                .arg(ports.input_port)
                .arg(ports.video_port)
                .arg(ports.audio_port)
                .arg(ports.virtual_display)
                .arg(want_gpu.isEmpty() ? QStringLiteral("(default)") : want_gpu);
            if (!extra_args.isEmpty()) {
                command_summary += QStringLiteral(" args=%1").arg(extra_args);
            }
            QMetaObject::invokeMethod(
                this,
                [this, instance_index, ports, resolved_gpu_label, command_summary] {
                    QString msg = QStringLiteral("SSH-starting remote executable (instance %1, port %2)")
                        .arg(instance_index)
                        .arg(ports.control_port);
                    if (!resolved_gpu_label.isEmpty()) {
                        msg += QStringLiteral(" (%1)").arg(resolved_gpu_label);
                    }
                    msg += QStringLiteral("…");
                    set_remote_status(msg);
                    append_log(remote_log_, command_summary);
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
            if (!ssh.stdout_text.trimmed().isEmpty()) {
                const auto startup_output = ssh.stdout_text.trimmed();
                const auto selected_gpu = startup_selected_gpu(startup_output);
                QMetaObject::invokeMethod(
                    this,
                    [this, startup_output, selected_gpu, want_gpu] {
                        append_log(remote_log_, startup_output);
                        if (!want_gpu.isEmpty() && !selected_gpu.isEmpty()
                            && selected_gpu != want_gpu && remote_gpu_ != nullptr) {
                            remote_gpu_->setText(selected_gpu);
                            append_log(
                                remote_log_,
                                QStringLiteral("Updated GPU override to %1 from startup match.")
                                    .arg(selected_gpu));
                        }
                    },
                    Qt::QueuedConnection);
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
                        QStringLiteral("Started new host instance on %1:%2%3")
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
                if (lobby_usable_for_request(info, resolved_gpu_id, process_gpu)) {
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
                        QStringLiteral("Reusing existing host instance on %1:%2%3%4")
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
    const auto want_gpu = remote_gpu_ != nullptr ? remote_gpu_->text().trimmed() : QString();
    const int ssh_port = remote_ssh_port_->value();
    const auto base_control =
        static_cast<std::uint16_t>(remote_base_control_port_->value());
    const auto base_input =
        static_cast<std::uint16_t>(remote_base_input_port_->value());
    const int tracked = remote_tracked_control_port_;

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
    if (tracked > 0 || want_gpu.isEmpty()) {
        const int control = tracked > 0 ? tracked : static_cast<int>(base_control);
        set_remote_status(QStringLiteral("Stopping remote host on control port %1…").arg(control));
    } else {
        set_remote_status(
            QStringLiteral("Finding remote host for GPU “%1” to stop…").arg(want_gpu));
    }

    std::thread([this,
                 ssh_host,
                 ssh_user,
                 password,
                 want_gpu,
                 ssh_port,
                 base_control,
                 base_input,
                 tracked] {
        int control = tracked > 0 ? tracked : static_cast<int>(base_control);

        if (tracked <= 0 && !want_gpu.isEmpty()) {
            int found_port = -1;
            for (int n = 0; n < 8; ++n) {
                const auto ports = remote_host_port_block(n, base_control, base_input);
                const auto gpu_cmd = QString::fromStdString(
                    remote_host_encode_gpu_query_shell(ports.control_port));
                const auto gpu_ssh = run_remote_ssh_command(
                    ssh_host, ssh_port, ssh_user, password, gpu_cmd, 15'000);
                if (!gpu_ssh.ok) {
                    continue;
                }
                const auto process_gpu = gpu_ssh.stdout_text.trimmed().toStdString();
                if (remote_host_gpu_preference_matches(want_gpu.toStdString(), process_gpu)) {
                    found_port = static_cast<int>(ports.control_port);
                    break;
                }
            }
            if (found_port < 0) {
                QMetaObject::invokeMethod(
                    this,
                    [this, want_gpu] {
                        remote_busy_ = false;
                        set_remote_status(
                            QStringLiteral("No running host_runner found for GPU “%1”.")
                                .arg(want_gpu));
                    },
                    Qt::QueuedConnection);
                return;
            }
            control = found_port;
            QMetaObject::invokeMethod(
                this,
                [this, control, want_gpu] {
                    set_remote_status(
                        QStringLiteral("Stopping GPU “%1” on control port %2…")
                            .arg(want_gpu)
                            .arg(control));
                },
                Qt::QueuedConnection);
        }

        const auto cmd = QString::fromStdString(remote_host_stop_shell(
            static_cast<std::uint16_t>(control)));
        const auto ssh = run_remote_ssh_command(
            ssh_host, ssh_port, ssh_user, password, cmd);
        QMetaObject::invokeMethod(
            this,
            [this, ssh, control, want_gpu] {
                remote_busy_ = false;
                if (!ssh.ok) {
                    set_remote_status(
                        QStringLiteral("Stop failed: %1%2")
                            .arg(ssh.error)
                            .arg(ssh.stderr_text.isEmpty()
                                ? QString()
                                : QStringLiteral("\n%1").arg(ssh.stderr_text)));
                } else {
                    QString msg = QStringLiteral("Stopped remote host on control port %1.")
                        .arg(control);
                    if (!want_gpu.isEmpty()) {
                        msg += QStringLiteral(" (GPU %1)").arg(want_gpu);
                    }
                    set_remote_status(msg);
                    remote_tracked_control_port_ = 0;
                    if (remote_users_ != nullptr) {
                        remote_users_->clear();
                    }
                }
                persist_settings_if_idle();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::refresh_remote_users() {
    if (remote_busy_) {
        set_remote_status(QStringLiteral("Remote action already running."));
        return;
    }
    const auto ssh_host = remote_ssh_host_->text().trimmed();
    const auto ssh_user = remote_ssh_user_->text().trimmed();
    const auto password = remote_ssh_password_->text();
    const int ssh_port = remote_ssh_port_->value();
    if (ssh_host.isEmpty() || ssh_user.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote users"),
            QStringLiteral("SSH host and user are required."));
        return;
    }
    if (password.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote users"),
            QStringLiteral("Enter the SSH password (it is not saved)."));
        return;
    }

    remote_busy_ = true;
    set_remote_status(QStringLiteral("Refreshing remote users…"));
    std::thread([this, ssh_host, ssh_user, password, ssh_port] {
        const auto cmd = QString::fromStdString(remote_host_list_presence_shell());
        const auto ssh = run_remote_ssh_command(
            ssh_host, ssh_port, ssh_user, password, cmd);
        QMetaObject::invokeMethod(
            this,
            [this, ssh] {
                remote_busy_ = false;
                if (remote_users_ == nullptr) {
                    return;
                }
                remote_users_->clear();
                if (!ssh.ok) {
                    set_remote_status(
                        QStringLiteral("Refresh users failed: %1%2")
                            .arg(ssh.error)
                            .arg(ssh.stderr_text.isEmpty()
                                ? QString()
                                : QStringLiteral("\n%1").arg(ssh.stderr_text)));
                    return;
                }
                const auto rows = remote_host_parse_presence_output(
                    ssh.stdout_text.toStdString());
                // Active first, then Connected (skip Connected covered by seated Active).
                std::vector<RemotePresenceRow> actives;
                std::vector<RemotePresenceRow> connected;
                for (const auto& row : rows) {
                    if (row.kind == "active") {
                        actives.push_back(row);
                    } else {
                        connected.push_back(row);
                    }
                }
                auto add_row = [&](const RemotePresenceRow& row, const QString& label) {
                    auto* item = new QListWidgetItem(label, remote_users_);
                    item->setData(kRemoteRoleKind, QString::fromStdString(row.kind));
                    item->setData(kRemoteRoleClientId, static_cast<qulonglong>(row.client_id));
                    item->setData(kRemoteRoleSlot, row.slot_index);
                };
                for (const auto& row : actives) {
                    const auto game = row.display_name.empty() ? row.game_id : row.display_name;
                    add_row(
                        row,
                        QStringLiteral("Active — %1 — %2 (slot %3)")
                            .arg(
                                QString::fromStdString(row.username),
                                QString::fromStdString(game))
                            .arg(row.slot_index));
                }
                for (const auto& row : connected) {
                    bool covered = false;
                    for (const auto& active : actives) {
                        if (active.slot_index == row.slot_index
                            && active.username == row.username
                            && row.seated) {
                            covered = true;
                            break;
                        }
                    }
                    if (covered) {
                        continue;
                    }
                    const auto phase = row.phase.empty()
                        ? (row.slot_index < 0 ? QStringLiteral("lobby") : QStringLiteral("session"))
                        : QString::fromStdString(row.phase);
                    add_row(
                        row,
                        QStringLiteral("Connected — %1 (client %2, %3)")
                            .arg(QString::fromStdString(row.username))
                            .arg(row.client_id)
                            .arg(phase));
                }
                set_remote_status(
                    QStringLiteral("Remote users: %1 row(s).")
                        .arg(remote_users_->count()));
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::kick_remote_user() {
    if (remote_busy_) {
        set_remote_status(QStringLiteral("Remote action already running."));
        return;
    }
    if (remote_users_ == nullptr || remote_users_->currentItem() == nullptr) {
        QMessageBox::information(
            this,
            QStringLiteral("Kick"),
            QStringLiteral("Select a remote user row first."));
        return;
    }
    const auto* item = remote_users_->currentItem();
    const auto kind = item->data(kRemoteRoleKind).toString();
    const auto client_id = static_cast<std::uint32_t>(item->data(kRemoteRoleClientId).toULongLong());
    const int slot = item->data(kRemoteRoleSlot).toInt();
    const auto label = item->text();

    const auto ssh_host = remote_ssh_host_->text().trimmed();
    const auto ssh_user = remote_ssh_user_->text().trimmed();
    const auto password = remote_ssh_password_->text();
    const int ssh_port = remote_ssh_port_->value();
    if (ssh_host.isEmpty() || ssh_user.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Kick"),
            QStringLiteral("SSH host and user are required."));
        return;
    }
    if (password.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Kick"),
            QStringLiteral("Enter the SSH password (it is not saved)."));
        return;
    }
    if (QMessageBox::question(
            this,
            QStringLiteral("Kick"),
            QStringLiteral("Kick “%1” on the remote host?").arg(label))
        != QMessageBox::Yes) {
        return;
    }

    remote_busy_ = true;
    set_remote_status(QStringLiteral("Kicking %1…").arg(label));
    std::thread([this, ssh_host, ssh_user, password, ssh_port, kind, client_id, slot, label] {
        const auto cmd = kind == QLatin1String("active")
            ? QString::fromStdString(remote_host_kick_active_shell(slot))
            : QString::fromStdString(remote_host_kick_connected_shell(client_id, slot));
        const auto ssh = run_remote_ssh_command(
            ssh_host, ssh_port, ssh_user, password, cmd);
        QMetaObject::invokeMethod(
            this,
            [this, ssh, label] {
                remote_busy_ = false;
                if (!ssh.ok) {
                    set_remote_status(
                        QStringLiteral("Kick failed: %1%2")
                            .arg(ssh.error)
                            .arg(ssh.stderr_text.isEmpty()
                                ? QString()
                                : QStringLiteral("\n%1").arg(ssh.stderr_text)));
                    return;
                }
                set_remote_status(QStringLiteral("Kick requested for %1.").arg(label));
                refresh_remote_users();
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace archstreamer::gui
