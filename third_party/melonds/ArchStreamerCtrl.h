/*
    ArchStreamer control socket for mid-session LAN host/connect and DS touch.
    Enabled when --archstreamer-ctrl <name> is passed (QLocalServer name).
*/

#ifndef ARCHSTREAMER_CTRL_H
#define ARCHSTREAMER_CTRL_H

#include <QHash>
#include <QObject>
#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>

class ArchStreamerCtrl : public QObject
{
    Q_OBJECT

public:
    struct ScreenRects {
        int windowW = 0;
        int windowH = 0;
        bool hasTop = false;
        int topX = 0, topY = 0, topW = 0, topH = 0;
        bool hasBot = false;
        int botX = 0, botY = 0, botW = 0, botH = 0;
    };

    explicit ArchStreamerCtrl(QObject* parent = nullptr);
    ~ArchStreamerCtrl() override;

    /** Bind a QLocalServer under @p serverName (abstract/local name, not a path). */
    bool start(const QString& serverName);

    static bool applyLanHost(const QString& playerName, int numPlayers);
    static bool applyLanConnect(const QString& playerName, const QString& host);
    static void applyLanEnd();
    /** Absolute DS stylus: x in [0,255], y in [0,191]. */
    static bool applyTouch(int x, int y);
    static void applyTouchEnd();
    /** Top/bottom AABBs in window pixels (updates with swap/emphasis). */
    static bool queryScreens(ScreenRects& out);
    /** Absolute pause (not a hotkey toggle). */
    static bool applyPause(bool paused);
    static bool applyPauseToggle();
    static bool queryPaused(bool& out);

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    void handleLine(QLocalSocket* sock, const QByteArray& line);
    void writeReply(QLocalSocket* sock, const QByteArray& reply);

    QLocalServer* server_ = nullptr;
    QHash<QLocalSocket*, QByteArray> pendingBySock_;
};

#endif // ARCHSTREAMER_CTRL_H
