#pragma once

#include <QString>

namespace archstreamer::gui {

struct RemoteSshResult {
    bool ok = false;
    int exit_code = -1;
    QString stdout_text;
    QString stderr_text;
    QString error;
};

/**
 * Run a remote shell command over OpenSSH with password auth via a temporary
 * SSH_ASKPASS helper (password never appears on argv).
 */
RemoteSshResult run_remote_ssh_command(
    const QString& host,
    int ssh_port,
    const QString& username,
    const QString& password,
    const QString& remote_command,
    int timeout_ms = 60'000);

/** Single-quote a string for POSIX sh. */
QString shell_single_quote(const QString& value);

} // namespace archstreamer::gui
