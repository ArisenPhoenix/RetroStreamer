/*
    ArchStreamer control socket for mid-session LAN host/connect.
    Enabled when --archstreamer-ctrl <name> is passed (QLocalServer name).
*/

#ifndef ARCHSTREAMER_CTRL_H
#define ARCHSTREAMER_CTRL_H

#include <QObject>
#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>

class ArchStreamerCtrl : public QObject
{
    Q_OBJECT

public:
    explicit ArchStreamerCtrl(QObject* parent = nullptr);
    ~ArchStreamerCtrl() override;

    /** Bind a QLocalServer under @p serverName (abstract/local name, not a path). */
    bool start(const QString& serverName);

    static bool applyLanHost(const QString& playerName, int numPlayers);
    static bool applyLanConnect(const QString& playerName, const QString& host);
    static void applyLanEnd();

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    void handleLine(QLocalSocket* sock, const QByteArray& line);
    void writeReply(QLocalSocket* sock, const QByteArray& reply);

    QLocalServer* server_ = nullptr;
    QByteArray pending_;
};

#endif // ARCHSTREAMER_CTRL_H
