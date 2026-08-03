/*
    ArchStreamer control socket for mid-session LAN host/connect and DS touch.

    Line-oriented commands (UTF-8, newline-terminated):
      LAN_HOST <player> [numplayers]
      LAN_CONNECT <player> <host>
      LAN_END
      TOUCH <x> <y>          // absolute DS coords; x 0-255, y 0-191
      TOUCH_END
      PING
    Replies: OK / ERR <message> / PONG
*/

#include "ArchStreamerCtrl.h"

#include <QStringList>
#include <algorithm>

#include "Config.h"
#include "LAN.h"
#include "main.h"

using namespace melonDS;

namespace {

LAN& lanInterface()
{
    return static_cast<LAN&>(MPInterface::Get());
}

} // namespace

ArchStreamerCtrl::ArchStreamerCtrl(QObject* parent) : QObject(parent) {}

ArchStreamerCtrl::~ArchStreamerCtrl()
{
    if (server_)
    {
        server_->close();
        QLocalServer::removeServer(server_->serverName());
    }
}

bool ArchStreamerCtrl::start(const QString& serverName)
{
    if (serverName.isEmpty())
        return false;

    if (server_)
    {
        server_->close();
        delete server_;
        server_ = nullptr;
    }

    QLocalServer::removeServer(serverName);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(serverName))
    {
        printf("ArchStreamerCtrl: failed to listen on '%s': %s\n",
               serverName.toUtf8().constData(),
               server_->errorString().toUtf8().constData());
        return false;
    }

    connect(server_, &QLocalServer::newConnection, this, &ArchStreamerCtrl::onNewConnection);
    printf("ArchStreamerCtrl: listening on '%s'\n", serverName.toUtf8().constData());
    return true;
}

bool ArchStreamerCtrl::applyLanHost(const QString& playerName, int numPlayers)
{
    QString player = playerName.trimmed();
    if (player.isEmpty())
        player = QString::fromStdString(Config::GetGlobalTable().GetString("LAN.PlayerName"));
    if (player.isEmpty())
        player = QStringLiteral("Player");

    if (numPlayers < 2)
        numPlayers = 2;
    if (numPlayers > 16)
        numPlayers = 16;

    if (MPInterface::GetType() == MPInterface_LAN)
        applyLanEnd();

    setMPInterface(MPInterface_LAN);
    if (!lanInterface().StartHost(player.toUtf8().constData(), numPlayers))
    {
        printf("ArchStreamerCtrl: StartHost failed\n");
        setMPInterface(MPInterface_Local);
        return false;
    }

    auto cfg = Config::GetGlobalTable();
    cfg.SetString("LAN.PlayerName", player.toStdString());
    cfg.SetInt("LAN.HostNumPlayers", numPlayers);
    Config::Save();
    printf("ArchStreamerCtrl: LAN host '%s' (%d players)\n",
           player.toUtf8().constData(), numPlayers);
    return true;
}

bool ArchStreamerCtrl::applyLanConnect(const QString& playerName, const QString& host)
{
    QString player = playerName.trimmed();
    if (player.isEmpty())
        player = QString::fromStdString(Config::GetGlobalTable().GetString("LAN.PlayerName"));
    if (player.isEmpty())
        player = QStringLiteral("Player");

    QString hostAddr = host.trimmed();
    if (hostAddr.isEmpty())
        hostAddr = QStringLiteral("127.0.0.1");

    if (MPInterface::GetType() == MPInterface_LAN)
        applyLanEnd();

    setMPInterface(MPInterface_LAN);
    if (!lanInterface().StartClient(player.toUtf8().constData(), hostAddr.toUtf8().constData()))
    {
        printf("ArchStreamerCtrl: StartClient failed (host=%s)\n", hostAddr.toUtf8().constData());
        setMPInterface(MPInterface_Local);
        return false;
    }

    auto cfg = Config::GetGlobalTable();
    cfg.SetString("LAN.PlayerName", player.toStdString());
    Config::Save();
    printf("ArchStreamerCtrl: LAN connect '%s' -> %s\n",
           player.toUtf8().constData(), hostAddr.toUtf8().constData());
    return true;
}

void ArchStreamerCtrl::applyLanEnd()
{
    if (MPInterface::GetType() == MPInterface_LAN)
        lanInterface().EndSession();
    setMPInterface(MPInterface_Local);
    printf("ArchStreamerCtrl: LAN session ended\n");
}

bool ArchStreamerCtrl::applyTouch(int x, int y)
{
    EmuInstance* inst = firstEmuInstance();
    if (!inst)
        return false;
    const int cx = std::clamp(x, 0, 255);
    const int cy = std::clamp(y, 0, 191);
    inst->touchScreen(cx, cy);
    return true;
}

void ArchStreamerCtrl::applyTouchEnd()
{
    if (EmuInstance* inst = firstEmuInstance())
        inst->releaseScreen();
}

void ArchStreamerCtrl::onNewConnection()
{
    while (server_ && server_->hasPendingConnections())
    {
        QLocalSocket* sock = server_->nextPendingConnection();
        if (!sock)
            continue;
        connect(sock, &QLocalSocket::readyRead, this, &ArchStreamerCtrl::onReadyRead);
        connect(sock, &QLocalSocket::disconnected, this, [this, sock]() {
            pendingBySock_.remove(sock);
            sock->deleteLater();
        });
    }
}

void ArchStreamerCtrl::onReadyRead()
{
    auto* sock = qobject_cast<QLocalSocket*>(sender());
    if (!sock)
        return;

    QByteArray& pending = pendingBySock_[sock];
    pending.append(sock->readAll());
    int idx;
    while ((idx = pending.indexOf('\n')) >= 0)
    {
        QByteArray line = pending.left(idx).trimmed();
        pending.remove(0, idx + 1);
        if (!line.isEmpty())
            handleLine(sock, line);
    }
}

void ArchStreamerCtrl::writeReply(QLocalSocket* sock, const QByteArray& reply)
{
    if (!sock)
        return;
    sock->write(reply);
    sock->write("\n");
    sock->flush();
}

void ArchStreamerCtrl::handleLine(QLocalSocket* sock, const QByteArray& line)
{
    const QString text = QString::fromUtf8(line).trimmed();
    const QStringList parts = text.split(QChar(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return;

    const QString cmd = parts[0].toUpper();
    if (cmd == QStringLiteral("PING"))
    {
        writeReply(sock, "PONG");
        return;
    }

    if (cmd == QStringLiteral("LAN_END"))
    {
        applyLanEnd();
        writeReply(sock, "OK");
        return;
    }

    if (cmd == QStringLiteral("LAN_HOST"))
    {
        QString player = parts.size() >= 2 ? parts[1] : QString();
        int num = 2;
        if (parts.size() >= 3)
            num = parts[2].toInt();
        if (applyLanHost(player, num))
            writeReply(sock, "OK");
        else
            writeReply(sock, "ERR StartHost failed");
        return;
    }

    if (cmd == QStringLiteral("LAN_CONNECT"))
    {
        if (parts.size() < 3)
        {
            writeReply(sock, "ERR usage: LAN_CONNECT <player> <host>");
            return;
        }
        if (applyLanConnect(parts[1], parts[2]))
            writeReply(sock, "OK");
        else
            writeReply(sock, "ERR StartClient failed");
        return;
    }

    if (cmd == QStringLiteral("TOUCH"))
    {
        if (parts.size() < 3)
        {
            writeReply(sock, "ERR usage: TOUCH <x> <y>");
            return;
        }
        bool okX = false;
        bool okY = false;
        const int x = parts[1].toInt(&okX);
        const int y = parts[2].toInt(&okY);
        if (!okX || !okY)
        {
            writeReply(sock, "ERR TOUCH coords");
            return;
        }
        if (applyTouch(x, y))
            writeReply(sock, "OK");
        else
            writeReply(sock, "ERR no emu instance");
        return;
    }

    if (cmd == QStringLiteral("TOUCH_END"))
    {
        applyTouchEnd();
        writeReply(sock, "OK");
        return;
    }

    writeReply(sock, "ERR unknown command");
}
