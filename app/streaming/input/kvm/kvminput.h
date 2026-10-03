/**
 * KVM input transport -- added by the moonlight-kvm-input fork.
 *
 * Redirects mouse and keyboard input to a GL.iNet KVM's JSON WebSocket HID API
 * so it reaches the controlled PC as real USB HID, while Moonlight still
 * carries the video. See docs/fork-design.md in the moonlight-kvm-input repo.
 *
 * The library-side seam is moonlight-common-c/src/KvmInputHooks.h: while hooks
 * are installed, LiSend* calls the handlers below INSTEAD of sending over the
 * GameStream protocol. The two paths are mutually exclusive by construction.
 *
 * THREADING (read this before changing anything)
 *   The hooks are NOT called from a single thread. SDL events are normally
 *   pumped on the main thread, but the ffmpeg renderers call
 *   Session::flushWindowEvents() from decoder threads, which pumps SDL and can
 *   deliver input there. Measured: a mouse move arrived on the connection
 *   thread during startup.
 *
 *   QWebSocket and QTimer are thread-affine, so touching them from a foreign
 *   thread silently breaks input (Qt refuses the cross-thread call). Qt posted
 *   events are not a way out either: the streaming loop never pumps them --
 *   QCoreApplication::processEvents() runs once, before the SDL window exists.
 *
 *   So the hook entry points do no Qt work at all. They append to a
 *   mutex-guarded queue, and the transport's own thread drains it from a timer
 *   that runs for the lifetime of the connection. Every event therefore leaves
 *   on one thread, in order, which also means a key-down and its key-up cannot
 *   be reordered -- the failure that would strand a key on the controlled PC.
 *
 * FAILURE POLICY
 *   If the transport is not connected, queued events are DROPPED, never
 *   forwarded to GameStream. Falling back would put synthetic input on the
 *   controlled machine mid-session, which is the exact hazard this design
 *   exists to remove. Errors are surfaced to the UI instead.
 */

#ifndef KVMINPUT_H
#define KVMINPUT_H

#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QWebSocket>

#include <atomic>

class KvmInput : public QObject
{
    Q_OBJECT

public:
    static KvmInput* instance();

    /**
     * Connect to the KVM and install the input hooks. Must be called on the
     * main thread, where the object is created and keeps its affinity.
     *
     * @param host  KVM hostname or address (`glkvm`, or an IP)
     * @param token device auth token
     * @param error set to a human-readable reason on failure
     * @return true if the transport was started and hooks installed
     */
    bool start(const QString& host, const QString& token, QString* error);

    /** Release anything still held, uninstall hooks, close the socket. */
    void stop();

    bool isActive() const { return m_Active.load(); }

    // --- hook targets; see KvmInputHooks.h. Callable from ANY thread. ----
    int mouseMove(short deltaX, short deltaY);
    int mousePosition(short x, short y, short referenceWidth, short referenceHeight);
    int mouseButton(char action, int button);
    int keyboard(short keyCode, char keyAction, char modifiers, char flags);
    int highResScroll(short scrollAmount);
    int highResHScroll(short scrollAmount);
    int dropInput(const char* kind);

private slots:
    void flushPendingInput();
    void onConnected();
    void onDisconnected();
    void onSslErrors(const QList<QSslError>& errors);
    void onError(QAbstractSocket::SocketError error);

private:
    explicit KvmInput(QObject* parent = nullptr);
    ~KvmInput() override;

    // Owning thread only.
    void sendEvent(const QString& eventType, const QJsonObject& event);
    void releaseAllHeld();
    int dropInput(const QString& kind);
    static QString keyNameForKeyCode(short keyCode, bool* known);
    static QString buttonNameForButton(int button, bool* known);

    /**
     * A discrete input event, queued by a hook on whatever thread delivered it
     * and sent by the owning thread. Keys and buttons share one queue so their
     * relative order is preserved -- a reordered key-down/key-up pair would
     * strand a key on the controlled PC.
     */
    struct PendingAction {
        enum Kind { Key, Button };
        Kind kind;
        QString name;
        bool down;
    };

    QWebSocket m_Socket;
    QTimer m_FlushTimer;

    std::atomic<bool> m_Active;
    bool m_Connected;
    bool m_WarnedNotConnected;
    QString m_Host;
    QString m_LastError;

    // Guarded by m_Mutex: appended by the hooks on any thread, drained by the
    // timer on the owning thread.
    mutable QMutex m_Mutex;
    int m_PendingDeltaX;
    int m_PendingDeltaY;
    int m_PendingWheelV;
    int m_PendingWheelH;
    QQueue<PendingAction> m_PendingActions;
    QSet<QString> m_PendingDrops;
    QSet<QString> m_LoggedTypes;

    // Owning thread only.
    QSet<QString> m_HeldKeys;
    QSet<QString> m_HeldButtons;
};

#endif // KVMINPUT_H
