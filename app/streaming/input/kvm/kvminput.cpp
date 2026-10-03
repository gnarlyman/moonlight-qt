/**
 * KVM input transport. See kvminput.h for the contract, especially the
 * THREADING section -- the hooks are called from more than one thread.
 */

#include "kvminput.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QSslConfiguration>
#include <QSslError>

#include <utility>

#include <Limelight.h>
#include <KvmInputHooks.h>

// Mirrors LI_WHEEL_DELTA in moonlight-common-c/src/InputStream.c:53. A "click"
// of the wheel is 120 high-resolution units; the KVM's mouse_wheel wants clicks.
static const int KVM_WHEEL_DELTA = 120;

// The transport's own thread drains queued input at this interval. Pointer
// motion is coalesced over it; discrete events (keys, buttons) go out on the
// next tick, so worst-case added latency is this value.
static const int KVM_FLUSH_INTERVAL_MS = 5;

// How long to wait for the WebSocket upgrade before giving up on start().
static const int KVM_CONNECT_TIMEOUT_MS = 5000;

// Defensive cap, so a stalled drain cannot grow the queue without bound.
static const int KVM_MAX_QUEUED_ACTIONS = 4096;

static KvmInput* s_Instance = nullptr;

// ---------------------------------------------------------------------------
// Hook table: these are what moonlight-common-c calls from LiSend*. They do no
// Qt work -- only mutex-guarded state -- so they are safe on any thread.
// ---------------------------------------------------------------------------

static int kvmHookMouseMove(short deltaX, short deltaY)
{
    return s_Instance ? s_Instance->mouseMove(deltaX, deltaY) : 0;
}

static int kvmHookMousePosition(short x, short y, short referenceWidth, short referenceHeight)
{
    return s_Instance ? s_Instance->mousePosition(x, y, referenceWidth, referenceHeight) : 0;
}

static int kvmHookMouseButton(char action, int button)
{
    return s_Instance ? s_Instance->mouseButton(action, button) : 0;
}

static int kvmHookKeyboard(short keyCode, char keyAction, char modifiers, char flags)
{
    return s_Instance ? s_Instance->keyboard(keyCode, keyAction, modifiers, flags) : 0;
}

static int kvmHookHighResScroll(short scrollAmount)
{
    return s_Instance ? s_Instance->highResScroll(scrollAmount) : 0;
}

static int kvmHookHighResHScroll(short scrollAmount)
{
    return s_Instance ? s_Instance->highResHScroll(scrollAmount) : 0;
}

static int kvmHookDropInput(const char* kind)
{
    return s_Instance ? s_Instance->dropInput(kind) : 0;
}

// ---------------------------------------------------------------------------

KvmInput::KvmInput(QObject* parent)
    : QObject(parent),
      m_Active(false),
      m_Connected(false),
      m_WarnedNotConnected(false),
      m_PendingDeltaX(0),
      m_PendingDeltaY(0),
      m_PendingWheelV(0),
      m_PendingWheelH(0)
{
    // Runs for the whole connection. The hooks must never start or stop a
    // timer, because they can be called from a thread that does not own it.
    m_FlushTimer.setInterval(KVM_FLUSH_INTERVAL_MS);
    m_FlushTimer.setSingleShot(false);
    connect(&m_FlushTimer, &QTimer::timeout, this, &KvmInput::flushPendingInput);

    connect(&m_Socket, &QWebSocket::connected, this, &KvmInput::onConnected);
    connect(&m_Socket, &QWebSocket::disconnected, this, &KvmInput::onDisconnected);
    connect(&m_Socket, &QWebSocket::errorOccurred, this, &KvmInput::onError);
    connect(&m_Socket, &QWebSocket::sslErrors, this, &KvmInput::onSslErrors);
}

KvmInput::~KvmInput()
{
    // Deliberately not calling stop(): hooks must already be cleared, and a
    // release on a half-destroyed socket is worse than nothing.
    s_Instance = nullptr;
}

KvmInput* KvmInput::instance()
{
    if (s_Instance == nullptr) {
        // The hooks run from the SDL input loop on the main thread, and the
        // socket and timer below are thread-affine, so this object must be
        // created here. Creating it on a worker leaves input silently dead.
        Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
        s_Instance = new KvmInput();
    }
    return s_Instance;
}

bool KvmInput::start(const QString& host, const QString& token, QString* error)
{
    if (m_Active.load()) {
        if (error) {
            *error = tr("KVM input is already running");
        }
        return false;
    }

    if (host.isEmpty()) {
        if (error) {
            *error = tr("No GLKVM host specified");
        }
        return false;
    }

    m_Host = host;
    {
        QMutexLocker locker(&m_Mutex);
        m_LastError.clear();
    }

    QUrl url;
    url.setScheme(QStringLiteral("wss"));
    url.setHost(host);
    url.setPath(QStringLiteral("/api/ws"));

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("stream"), QStringLiteral("0"));
    if (!token.isEmpty()) {
        query.addQueryItem(QStringLiteral("auth_token"), token);
    }
    url.setQuery(query);

    QNetworkRequest request(url);

    // Present the token BOTH ways, so the device's auth mechanism is not a
    // variable in whether input works:
    //   - upstream kvmd reads the cookie `auth_token` (_check_token), and has no
    //     query-parameter checker at all;
    //   - the GLKVM web UI passes it in the URL, so the fork may accept that.
    // Sending both satisfies either.
    if (!token.isEmpty()) {
        request.setRawHeader("Cookie",
                             QStringLiteral("auth_token=%1").arg(token).toUtf8());
    }

    // The device ships a certificate that expired in 1979 (see
    // docs/live-test-results.md). Verification can never succeed, so skip it;
    // the connection is authenticated by the token.
    QSslConfiguration ssl = request.sslConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(ssl);

    m_Socket.open(request);

    // Wait for the upgrade so a bad host or token fails here, loudly, rather
    // than silently dropping every event for the rest of the session.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;

    QMetaObject::Connection connOk = connect(&m_Socket, &QWebSocket::connected, &loop, &QEventLoop::quit);
    QMetaObject::Connection connErr = connect(&m_Socket, &QWebSocket::errorOccurred,
                                              &loop, &QEventLoop::quit);
    QMetaObject::Connection connTo = connect(&timeout, &QTimer::timeout, this, [&timedOut, &loop]() {
        timedOut = true;
        loop.quit();
    });

    timeout.start(KVM_CONNECT_TIMEOUT_MS);
    loop.exec();
    disconnect(connOk);
    disconnect(connErr);
    disconnect(connTo);

    if (!m_Connected) {
        if (error) {
            QMutexLocker locker(&m_Mutex);
            *error = timedOut
                ? tr("Timed out connecting to %1").arg(host)
                : tr("Could not connect to %1: %2").arg(host, m_LastError);
        }
        m_Socket.abort();
        return false;
    }

    // Install the hooks last, so we never intercept input we cannot deliver.
    static const KVM_INPUT_HOOKS hooks = {
        kvmHookMouseMove,
        kvmHookMousePosition,
        kvmHookMouseButton,
        kvmHookKeyboard,
        kvmHookHighResScroll,
        kvmHookHighResHScroll,
        kvmHookDropInput,
    };

    m_Active.store(true);
    LiSetKvmInputHooks(&hooks);
    m_FlushTimer.start();

    return true;
}

void KvmInput::stop()
{
    if (!m_Active.load()) {
        return;
    }

    // Stop accepting input first.
    LiSetKvmInputHooks(nullptr);
    m_Active.store(false);

    m_FlushTimer.stop();

    // Drop anything queued but not yet sent: the device never saw it, so there
    // is nothing to unwind.
    {
        QMutexLocker locker(&m_Mutex);
        m_PendingDeltaX = m_PendingDeltaY = 0;
        m_PendingWheelV = m_PendingWheelH = 0;
        m_PendingActions.clear();
        m_PendingDrops.clear();
    }

    // Release anything we did send, while the socket is still open. This is the
    // same job raiseAllKeys() does for the GameStream path; without it, keys
    // stay held on the controlled machine.
    releaseAllHeld();

    m_Connected = false;
    m_Socket.close();
}


// ---------------------------------------------------------------------------
// Hook entry points: queue only, no Qt calls. Safe on any thread.
// ---------------------------------------------------------------------------

int KvmInput::mouseMove(short deltaX, short deltaY)
{
    if (!m_Active.load()) {
        return 0;
    }

    QMutexLocker locker(&m_Mutex);
    m_PendingDeltaX += deltaX;
    m_PendingDeltaY += deltaY;
    return 0;
}

int KvmInput::mousePosition(short x, short y, short referenceWidth, short referenceHeight)
{
    // Only reachable in absolute mouse mode. The device is in relative mode
    // (mouse.absolute: false, output usb_rel) so this should not fire; drop it
    // rather than leak it to GameStream. Translate here if absolute mode is
    // ever enabled.
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(referenceWidth);
    Q_UNUSED(referenceHeight);
    return dropInput(QStringLiteral("absolute mouse (device is in relative mode)"));
}

int KvmInput::mouseButton(char action, int button)
{
    if (!m_Active.load()) {
        return 0;
    }

    bool known = false;
    const QString name = buttonNameForButton(button, &known);
    if (!known) {
        return dropInput(QStringLiteral("unknown mouse button"));
    }

    QMutexLocker locker(&m_Mutex);
    if (m_PendingActions.size() < KVM_MAX_QUEUED_ACTIONS) {
        m_PendingActions.enqueue({ PendingAction::Button, name, action != BUTTON_ACTION_RELEASE });
    }
    return 0;
}

int KvmInput::keyboard(short keyCode, char keyAction, char modifiers, char flags)
{
    if (!m_Active.load()) {
        return 0;
    }

    // The modifiers bitmask exists so GFE can normalise the event server-side.
    // The KVM applies no such correction, and modifier keys already arrive as
    // their own discrete key events (LSHIFT -> 0xA0, RCTRL -> 0xA3, ...), so the
    // mask is redundant here.
    Q_UNUSED(modifiers);
    Q_UNUSED(flags);

    bool known = false;
    const QString name = keyNameForKeyCode(keyCode, &known);
    if (!known) {
        return dropInput(QStringLiteral("unmapped keycode 0x%1").arg(keyCode & 0xFFFF, 0, 16));
    }

    QMutexLocker locker(&m_Mutex);
    if (m_PendingActions.size() < KVM_MAX_QUEUED_ACTIONS) {
        m_PendingActions.enqueue({ PendingAction::Key, name, keyAction != KEY_ACTION_UP });
    }
    return 0;
}

int KvmInput::highResScroll(short scrollAmount)
{
    if (!m_Active.load()) {
        return 0;
    }
    QMutexLocker locker(&m_Mutex);
    m_PendingWheelV += scrollAmount;
    return 0;
}

int KvmInput::highResHScroll(short scrollAmount)
{
    if (!m_Active.load()) {
        return 0;
    }
    QMutexLocker locker(&m_Mutex);
    m_PendingWheelH += scrollAmount;
    return 0;
}

int KvmInput::dropInput(const char* kind)
{
    if (!m_Active.load()) {
        return 0;
    }
    return dropInput(QString::fromLatin1(kind ? kind : "input"));
}

int KvmInput::dropInput(const QString& kind)
{
    // Queued rather than logged: this may run on any thread, and the owning
    // thread does the logging in flushPendingInput().
    QMutexLocker locker(&m_Mutex);
    m_PendingDrops.insert(kind);
    return 0;
}

// ---------------------------------------------------------------------------
// Owning thread
// ---------------------------------------------------------------------------

void KvmInput::sendEvent(const QString& eventType, const QJsonObject& event)
{
    if (!m_Connected) {
        // Drop rather than fall back. See the failure policy in kvminput.h.
        if (!m_WarnedNotConnected) {
            m_WarnedNotConnected = true;
            qWarning() << "KVM input: transport is not connected -- dropping input";
        }
        return;
    }

    if (!m_LoggedTypes.contains(eventType)) {
        m_LoggedTypes.insert(eventType);
        qInfo() << "KVM input: first" << eventType << "event sent";
    }

    QJsonObject message;
    message[QStringLiteral("event_type")] = eventType;
    message[QStringLiteral("event")] = event;

    m_Socket.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

/** Split a delta pair into chunks the KVM accepts (+/-127 per axis). */
static QJsonArray splitDeltaPair(int deltaX, int deltaY)
{
    QJsonArray deltas;
    while (deltaX != 0 || deltaY != 0) {
        const int chunkX = qBound(-127, deltaX, 127);
        const int chunkY = qBound(-127, deltaY, 127);

        QJsonObject delta;
        delta[QStringLiteral("x")] = chunkX;
        delta[QStringLiteral("y")] = chunkY;
        deltas.append(delta);

        deltaX -= chunkX;
        deltaY -= chunkY;
    }
    return deltas;
}

void KvmInput::flushPendingInput()
{
    if (!m_Active.load()) {
        return;
    }

    int deltaX, deltaY, wheelV, wheelH;
    QQueue<PendingAction> actions;
    QSet<QString> drops;

    {
        QMutexLocker locker(&m_Mutex);
        deltaX = m_PendingDeltaX;
        deltaY = m_PendingDeltaY;
        wheelV = m_PendingWheelV / KVM_WHEEL_DELTA;
        wheelH = m_PendingWheelH / KVM_WHEEL_DELTA;

        m_PendingDeltaX = 0;
        m_PendingDeltaY = 0;
        // Keep the sub-click remainder so slow scroll wheels still register.
        m_PendingWheelV -= wheelV * KVM_WHEEL_DELTA;
        m_PendingWheelH -= wheelH * KVM_WHEEL_DELTA;

        actions.swap(m_PendingActions);
        drops.swap(m_PendingDrops);
    }

    if (!m_Connected) {
        // Everything drained above is discarded on purpose.
        if (!actions.isEmpty() || deltaX != 0 || deltaY != 0 || wheelV != 0 || wheelH != 0) {
            if (!m_WarnedNotConnected) {
                m_WarnedNotConnected = true;
                qWarning() << "KVM input: transport is not connected -- dropping input";
            }
        }
        return;
    }

    for (const QString& kind : std::as_const(drops)) {
        qWarning() << "KVM input: dropped" << kind;
    }

    // Discrete events in the order they were generated, so a key-down and its
    // key-up can never be reordered.
    for (const PendingAction& action : std::as_const(actions)) {
        QJsonObject event;
        if (action.kind == PendingAction::Key) {
            event[QStringLiteral("key")] = action.name;
            event[QStringLiteral("state")] = action.down;

            // NOTE: `finish` is deliberately never sent. It is only needed to
            // release a key in a single event, and we translate moonlight's
            // explicit down/up events instead. That matters because the device
            // SILENTLY IGNORES `finish` for the eight modifiers and PrintScreen
            // (kvmd BaseHid.send_key_event), which would strand them held. See
            // docs/live-test-results.md.
            sendEvent(QStringLiteral("key"), event);

            if (action.down) {
                m_HeldKeys.insert(action.name);
            }
            else {
                m_HeldKeys.remove(action.name);
            }
        }
        else {
            event[QStringLiteral("button")] = action.name;
            event[QStringLiteral("state")] = action.down;
            sendEvent(QStringLiteral("mouse_button"), event);

            if (action.down) {
                m_HeldButtons.insert(action.name);
            }
            else {
                m_HeldButtons.remove(action.name);
            }
        }
    }

    if (deltaX != 0 || deltaY != 0) {
        QJsonObject event;
        event[QStringLiteral("delta")] = splitDeltaPair(deltaX, deltaY);
        sendEvent(QStringLiteral("mouse_relative"), event);
    }

    if (wheelV != 0 || wheelH != 0) {
        QJsonObject event;
        event[QStringLiteral("delta")] = splitDeltaPair(wheelH, wheelV);
        sendEvent(QStringLiteral("mouse_wheel"), event);
    }
}

void KvmInput::releaseAllHeld()
{
    for (const QString& key : std::as_const(m_HeldKeys)) {
        QJsonObject event;
        event[QStringLiteral("key")] = key;
        event[QStringLiteral("state")] = false;
        sendEvent(QStringLiteral("key"), event);
    }
    m_HeldKeys.clear();

    for (const QString& button : std::as_const(m_HeldButtons)) {
        QJsonObject event;
        event[QStringLiteral("button")] = button;
        event[QStringLiteral("state")] = false;
        sendEvent(QStringLiteral("mouse_button"), event);
    }
    m_HeldButtons.clear();
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

void KvmInput::onConnected()
{
    m_Connected = true;
}

void KvmInput::onDisconnected()
{
    m_Connected = false;
    if (m_Active.load()) {
        QMutexLocker locker(&m_Mutex);
        m_LastError = tr("Disconnected from %1").arg(m_Host);
        qWarning() << "KVM input: disconnected from" << m_Host;
    }
}

void KvmInput::onError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    m_Connected = false;
    QMutexLocker locker(&m_Mutex);
    m_LastError = m_Socket.errorString();
}

void KvmInput::onSslErrors(const QList<QSslError>& errors)
{
    // Peer verification is disabled up front, but ignore explicitly too: the
    // device certificate expired in 1979 and must never block input.
    Q_UNUSED(errors);
    m_Socket.ignoreSslErrors();
}

// ---------------------------------------------------------------------------
// Translation tables
// ---------------------------------------------------------------------------

QString KvmInput::buttonNameForButton(int button, bool* known)
{
    *known = true;
    switch (button) {
    case BUTTON_LEFT:   return QStringLiteral("left");
    case BUTTON_MIDDLE: return QStringLiteral("middle");
    case BUTTON_RIGHT:  return QStringLiteral("right");
    case BUTTON_X1:     return QStringLiteral("up");
    case BUTTON_X2:     return QStringLiteral("down");
    default:
        *known = false;
        return QString();
    }
}

/**
 * Map a moonlight keycode to a KVM `web_name`.
 *
 * moonlight sends Windows virtual-key codes with 0x8000 set (see
 * keyboard.cpp's GET_KEYPRESS_CODE / `keyCode |= 0x8000`), so the flag is
 * masked off here. The names on the right are the authoritative set from
 * keymap.csv, the same catalogue moonlight-kvm-input validates against.
 */
QString KvmInput::keyNameForKeyCode(short keyCode, bool* known)
{
    const short vk = keyCode & 0x7FFF;
    *known = true;

    // Letters and digits
    if (vk >= 'A' && vk <= 'Z') {
        return QStringLiteral("Key%1").arg(QChar(vk));
    }
    if (vk >= '0' && vk <= '9') {
        return QStringLiteral("Digit%1").arg(QChar(vk));
    }

    // Numpad digits
    if (vk >= 0x60 && vk <= 0x69) {
        return QStringLiteral("Numpad%1").arg(vk - 0x60);
    }

    // Function keys
    if (vk >= 0x70 && vk <= 0x7B) {
        return QStringLiteral("F%1").arg(vk - 0x70 + 1);
    }
    if (vk >= 0x7C && vk <= 0x87) {
        return QStringLiteral("F%1").arg(vk - 0x7C + 13);
    }

    switch (vk) {
    // Editing and navigation
    case 0x08: return QStringLiteral("Backspace");
    case 0x09: return QStringLiteral("Tab");
    case 0x0D: return QStringLiteral("Enter");
    case 0x1B: return QStringLiteral("Escape");
    case 0x20: return QStringLiteral("Space");
    case 0x21: return QStringLiteral("PageUp");
    case 0x22: return QStringLiteral("PageDown");
    case 0x23: return QStringLiteral("End");
    case 0x24: return QStringLiteral("Home");
    case 0x25: return QStringLiteral("ArrowLeft");
    case 0x26: return QStringLiteral("ArrowUp");
    case 0x27: return QStringLiteral("ArrowRight");
    case 0x28: return QStringLiteral("ArrowDown");
    case 0x2D: return QStringLiteral("Insert");
    case 0x2E: return QStringLiteral("Delete");

    // Locks and system
    case 0x13: return QStringLiteral("Pause");
    case 0x14: return QStringLiteral("CapsLock");
    case 0x2C: return QStringLiteral("PrintScreen");
    case 0x5D: return QStringLiteral("ContextMenu");
    case 0x90: return QStringLiteral("NumLock");
    case 0x91: return QStringLiteral("ScrollLock");

    // Modifiers, left and right. These arrive as ordinary key events, and
    // `finish` must never be used on them (the device ignores it).
    case 0x10:
    case 0xA0: return QStringLiteral("ShiftLeft");
    case 0xA1: return QStringLiteral("ShiftRight");
    case 0x11:
    case 0xA2: return QStringLiteral("ControlLeft");
    case 0xA3: return QStringLiteral("ControlRight");
    case 0x12:
    case 0xA4: return QStringLiteral("AltLeft");
    case 0xA5: return QStringLiteral("AltRight");
    case 0x5B: return QStringLiteral("MetaLeft");
    case 0x5C: return QStringLiteral("MetaRight");

    // OEM punctuation
    case 0xBA: return QStringLiteral("Semicolon");
    case 0xBB: return QStringLiteral("Equal");
    case 0xBC: return QStringLiteral("Comma");
    case 0xBD: return QStringLiteral("Minus");
    case 0xBE: return QStringLiteral("Period");
    case 0xBF: return QStringLiteral("Slash");
    case 0xC0: return QStringLiteral("Backquote");
    case 0xDB: return QStringLiteral("BracketLeft");
    case 0xDC: return QStringLiteral("Backslash");
    case 0xDD: return QStringLiteral("BracketRight");
    case 0xDE: return QStringLiteral("Quote");
    case 0xE2: return QStringLiteral("IntlBackslash");

    // Numpad operators
    case 0x6A: return QStringLiteral("NumpadMultiply");
    case 0x6B: return QStringLiteral("NumpadAdd");
    case 0x6D: return QStringLiteral("NumpadSubtract");
    case 0x6E: return QStringLiteral("NumpadDecimal");
    case 0x6F: return QStringLiteral("NumpadDivide");

    // Media
    case 0xAD: return QStringLiteral("AudioVolumeMute");
    case 0xAE: return QStringLiteral("AudioVolumeDown");
    case 0xAF: return QStringLiteral("AudioVolumeUp");

    default:
        *known = false;
        return QString();
    }
}
