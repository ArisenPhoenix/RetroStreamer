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
        "Path to host_runner on the remote machine.\n"
        "If you paste a build directory, /host_runner is appended automatically.");
    remote_base_control_port_ = new QSpinBox(form_box);
    remote_base_control_port_->setRange(1, 65535);
    remote_base_control_port_->setValue(static_cast<int>(RemoteDefaultControlPort));
    remote_base_input_port_ = new QSpinBox(form_box);
    remote_base_input_port_->setRange(1, 65535);
    remote_base_input_port_->setValue(static_cast<int>(RemoteDefaultInputPort));

    form->addRow("SSH host", remote_ssh_host_);
    form->addRow("SSH user", remote_ssh_user_);
    form->addRow("SSH password", remote_ssh_password_);
    form->addRow("SSH port", remote_ssh_port_);
    form->addRow("Remote directory", remote_directory_);
    form->addRow("Remote ROM root", remote_rom_root_);
    form->addRow("host_runner path", remote_binary_);
    form->addRow("Base control port", remote_base_control_port_);
    form->addRow("Base input port", remote_base_input_port_);

    auto* buttons = new QHBoxLayout();
    auto* ensure = new QPushButton("Ensure Host", page);
    auto* stop = new QPushButton("Stop Host", page);
    buttons->addWidget(ensure);
    buttons->addWidget(stop);
    buttons->addStretch(1);

    remote_status_ = new QLabel(
        "Ensure Host probes the base control port, reuses a free lobby, or SSH-starts host_runner.",
        page);
    remote_status_->setWordWrap(true);
    remote_log_ = new QPlainTextEdit(page);
    remote_log_->setObjectName("remoteLog");
    remote_log_->setReadOnly(true);
    remote_log_->setMaximumBlockCount(500);

    connect(ensure, &QPushButton::clicked, this, [this] { ensure_remote_host(); });
    connect(stop, &QPushButton::clicked, this, [this] { stop_remote_host(); });
    for (auto* edit : {remote_ssh_host_, remote_ssh_user_, remote_directory_, remote_rom_root_, remote_binary_}) {
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
    const int ssh_port = remote_ssh_port_->value();
    const auto base_control =
        static_cast<std::uint16_t>(remote_base_control_port_->value());
    const auto base_input =
        static_cast<std::uint16_t>(remote_base_input_port_->value());

    if (ssh_host.isEmpty() || ssh_user.isEmpty() || directory.isEmpty() || rom_root.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote host"),
            QStringLiteral("SSH host, user, remote directory, and ROM root are required."));
        return;
    }
    if (password.isEmpty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Remote host"),
            QStringLiteral("Enter the SSH password (it is not saved)."));
        return;
    }

    // Persist form fields even if Ensure fails (password still excluded).
    persist_settings_if_idle();

    remote_busy_ = true;
    set_remote_status(QStringLiteral("Probing %1:%2…").arg(ssh_host).arg(base_control));

    std::thread([this,
                 ssh_host,
                 ssh_user,
                 password,
                 directory,
                 rom_root,
                 binary,
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

        ActiveSessionInfo info{};
        QString probe_error;
        const auto host_std = ssh_host.toStdString();
        if (try_probe_session(client_app_, host_std, base_control, &info, &probe_error)) {
            if (!remote_host_lobby_full(info.active_slots, info.max_slots)) {
                const QString slot_text = (info.active_slots && info.max_slots)
                    ? QStringLiteral(" (slots %1/%2)")
                        .arg(*info.active_slots)
                        .arg(*info.max_slots)
                    : QString();
                finish(
                    QStringLiteral("Reusing existing host on %1:%2%3")
                        .arg(ssh_host)
                        .arg(base_control)
                        .arg(slot_text),
                    true,
                    ssh_host,
                    base_control,
                    base_input,
                    base_control);
                return;
            }

            // Base lobby is full — find the next free overflow instance.
            for (int n = 1; n <= 8; ++n) {
                const auto ports = remote_host_port_block(
                    n, base_control, base_input);
                ActiveSessionInfo overflow{};
                QString overflow_error;
                if (try_probe_session(
                        client_app_, host_std, ports.control_port, &overflow, &overflow_error)
                    && !remote_host_lobby_full(overflow.active_slots, overflow.max_slots)) {
                    finish(
                        QStringLiteral("Reusing overflow host on %1:%2 (slots %3/%4)")
                            .arg(ssh_host)
                            .arg(ports.control_port)
                            .arg(overflow.active_slots.value_or(0))
                            .arg(overflow.max_slots.value_or(0)),
                        true,
                        ssh_host,
                        ports.control_port,
                        ports.input_port,
                        ports.control_port);
                    return;
                }
                if (!try_probe_session(
                        client_app_, host_std, ports.control_port, &overflow, &overflow_error)) {
                    // Need to start this overflow instance via SSH.
                    const auto cmd = QString::fromStdString(remote_host_start_shell(
                        directory.toStdString(),
                        binary.toStdString(),
                        rom_root.toStdString(),
                        ports));
                    QMetaObject::invokeMethod(
                        this,
                        [this, n, ports, cmd] {
                            set_remote_status(
                                QStringLiteral("Base lobby full — SSH-starting overflow instance %1 on port %2…")
                                    .arg(n)
                                    .arg(ports.control_port));
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
                        return;
                    }
                    // Wait briefly then probe.
                    for (int attempt = 0; attempt < 20; ++attempt) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        ActiveSessionInfo started{};
                        QString started_error;
                        if (try_probe_session(
                                client_app_,
                                host_std,
                                ports.control_port,
                                &started,
                                &started_error)) {
                            finish(
                                QStringLiteral("Started overflow host on %1:%2")
                                    .arg(ssh_host)
                                    .arg(ports.control_port),
                                true,
                                ssh_host,
                                ports.control_port,
                                ports.input_port,
                                ports.control_port);
                            return;
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
                    return;
                }
            }
            finish(
                QStringLiteral("All probed overflow instances are full."),
                false,
                {},
                0,
                0,
                0);
            return;
        }

        // Nothing on base port — SSH-start instance 0.
        const auto ports = remote_host_port_block(0, base_control, base_input);
        const auto cmd = QString::fromStdString(remote_host_start_shell(
            directory.toStdString(),
            binary.toStdString(),
            rom_root.toStdString(),
            ports));
        QMetaObject::invokeMethod(
            this,
            [this, probe_error, cmd] {
                set_remote_status(
                    QStringLiteral("No host on base port (%1) — SSH-starting host_runner…")
                        .arg(probe_error));
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
            return;
        }
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ActiveSessionInfo started{};
            QString started_error;
            if (try_probe_session(
                    client_app_, host_std, ports.control_port, &started, &started_error)) {
                finish(
                    QStringLiteral("Started host on %1:%2")
                        .arg(ssh_host)
                        .arg(ports.control_port),
                    true,
                    ssh_host,
                    ports.control_port,
                    ports.input_port,
                    ports.control_port);
                return;
            }
        }
        finish(
            QStringLiteral("SSH start reported success but control port %1 never answered")
                .arg(ports.control_port),
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
    const int tracked = remote_tracked_control_port_ > 0
        ? remote_tracked_control_port_
        : remote_base_control_port_->value();

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
    set_remote_status(QStringLiteral("Stopping remote host on control port %1…").arg(tracked));

    std::thread([this, ssh_host, ssh_user, password, directory, ssh_port, tracked] {
        const auto cmd = QString::fromStdString(
            remote_host_stop_shell(
                static_cast<std::uint16_t>(tracked),
                directory.toStdString()));
        const auto ssh = run_remote_ssh_command(
            ssh_host, ssh_port, ssh_user, password, cmd);
        QMetaObject::invokeMethod(
            this,
            [this, ssh, tracked] {
                remote_busy_ = false;
                if (ssh.ok) {
                    set_remote_status(
                        QStringLiteral("Stopped remote host on control port %1.").arg(tracked));
                } else {
                    set_remote_status(QStringLiteral("SSH stop failed: %1").arg(ssh.error));
                }
                persist_settings_if_idle();
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace archstreamer::gui
