#include "remote_ssh.hpp"

#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

namespace archstreamer::gui {
namespace {

QString read_process_output(QProcess& process) {
    return QString::fromLocal8Bit(process.readAllStandardOutput())
        + QString::fromLocal8Bit(process.readAllStandardError());
}

} // namespace

QString shell_single_quote(const QString& value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

RemoteSshResult run_remote_ssh_command(
    const QString& host,
    int ssh_port,
    const QString& username,
    const QString& password,
    const QString& remote_command,
    int timeout_ms) {
    RemoteSshResult result;
    if (host.trimmed().isEmpty()) {
        result.error = QStringLiteral("SSH host is empty");
        return result;
    }
    if (username.trimmed().isEmpty()) {
        result.error = QStringLiteral("SSH username is empty");
        return result;
    }
    if (password.isEmpty()) {
        result.error = QStringLiteral("SSH password is empty (not persisted — re-enter it)");
        return result;
    }

    QTemporaryDir askpass_dir;
    if (!askpass_dir.isValid()) {
        result.error = QStringLiteral("failed to create temporary askpass directory");
        return result;
    }

    const QString askpass_path = askpass_dir.filePath(QStringLiteral("askpass.sh"));
    QFile askpass(askpass_path);
    if (!askpass.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.error = QStringLiteral("failed to write askpass helper");
        return result;
    }
    // Password is only in this temp file for the duration of the SSH call.
    const QByteArray script =
        QByteArrayLiteral("#!/bin/sh\nprintf '%s\\n' ")
        + shell_single_quote(password).toUtf8()
        + QByteArrayLiteral("\n");
    askpass.write(script);
    askpass.close();
    askpass.setPermissions(
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SSH_ASKPASS"), askpass_path);
    env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
    env.insert(QStringLiteral("DISPLAY"), env.value(QStringLiteral("DISPLAY"), QStringLiteral(":0")));
    // Avoid inheriting an agent that might try keys first inconsistently.
    env.remove(QStringLiteral("SSH_AUTH_SOCK"));
    process.setProcessEnvironment(env);

    QStringList args;
    args << QStringLiteral("-p") << QString::number(ssh_port)
         << QStringLiteral("-o") << QStringLiteral("BatchMode=no")
         << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=accept-new")
         << QStringLiteral("-o") << QStringLiteral("PreferredAuthentications=password")
         << QStringLiteral("-o") << QStringLiteral("PubkeyAuthentication=no")
         << QStringLiteral("-o") << QStringLiteral("NumberOfPasswordPrompts=1")
         << (username.trimmed() + QLatin1Char('@') + host.trimmed())
         << remote_command;

    process.start(QStringLiteral("ssh"), args);
    if (!process.waitForStarted(5'000)) {
        result.error = QStringLiteral("failed to start ssh (is OpenSSH client installed?)");
        return result;
    }
    if (!process.waitForFinished(timeout_ms)) {
        process.kill();
        process.waitForFinished(3'000);
        result.error = QStringLiteral("ssh timed out");
        result.stderr_text = read_process_output(process);
        return result;
    }

    result.exit_code = process.exitCode();
    result.stdout_text = QString::fromLocal8Bit(process.readAllStandardOutput());
    result.stderr_text = QString::fromLocal8Bit(process.readAllStandardError());
    result.ok = process.exitStatus() == QProcess::NormalExit && result.exit_code == 0;
    if (!result.ok && result.error.isEmpty()) {
        result.error = result.stderr_text.trimmed();
        if (result.error.isEmpty()) {
            result.error = QStringLiteral("ssh exited with code %1").arg(result.exit_code);
        }
    }
    return result;
}

} // namespace archstreamer::gui
