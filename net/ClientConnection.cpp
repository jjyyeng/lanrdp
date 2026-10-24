#include "net/ClientConnection.h"

#include "net/Auth.h"
#include "net/FileSender.h"
#include "net/Session.h"
#include "protocol/H264Frame.h"
#include "protocol/ScreenFrame.h"
#include "viewer/DecoderWorker.h"

#include <QDateTime>
#include <QImage>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

#include <algorithm>

namespace lanrdp {

namespace {
// 控制端心跳 / 死链判定。与 HostServer 侧同值（3 s / 10 s）。
constexpr int kHeartbeatIntervalMs = 3000;
constexpr int kClientDeadTimeoutMs = 10000;

// 自动重连退避上限。1 → 2 → 4 → 8 → 16 → 30 s 封顶。
constexpr int kReconnectMaxDelayMs = 30000;

// H.264 解码器连续打开失败多少次才判「本机不可用」。
// 单次失败可能只是瞬态（系统临时没资源、别的程序正占着解码器），
// 一次性放弃会让画面永久黑屏；但一直重试又白烧 CPU，故取一个小上限。
constexpr int kMaxDecoderOpenFailures = 3;
} // namespace

ClientConnection::ClientConnection(QObject *parent)
    : QObject(parent)
{
    // 传输 id 从随机值起步：它是协议里的唯一标识，固定从 1 开始的话
    // 多条连接会撞在同一串 id 上，接收侧（按来源隔离后已无碍，但）
    // 任何依赖「id 全局唯一」的推断都会失效。取值只要求同连接内不重复。
    nextTransferId_ = QRandomGenerator::global()->generate();

    // 定时器**每连接一份**：控制端的判死与退避天然按连接记，连接数 ≤4 开销可忽略。
    heartbeatTimer_ = new QTimer(this);
    heartbeatTimer_->setInterval(kHeartbeatIntervalMs);
    connect(heartbeatTimer_, &QTimer::timeout, this, &ClientConnection::onHeartbeatTick);

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, &ClientConnection::onReconnectTimeout);
}

ClientConnection::~ClientConnection()
{
    // 退出路径必须先掐断网络信号：QTcpSocket 的析构会**同步**发 disconnected，
    // 而此刻上层（MainWindow）可能已在拆子控件 —— 让它回调进已半销毁的对象会崩。
    // （这条经验来自 MainWindow 析构里同等处理的那段注释。）
    if (session_ && session_->socket())
        session_->socket()->disconnect();

    if (heartbeatTimer_)
        heartbeatTimer_->stop();
    if (reconnectTimer_)
        reconnectTimer_->stop();

    if (fileSender_) {
        fileSender_->abort();
        delete fileSender_;
        fileSender_ = nullptr;
    }
    if (decoder_) {
        // stop() 内部 quit()+wait()，保证解码线程**已经退出**才 delete。
        // 不能只 delete —— QThread 仍在跑时被析构会直接 abort 进程。
        decoder_->stop();
        delete decoder_;
        decoder_ = nullptr;
    }
    // session_ / authClient_ / socket 都是 this 的子对象，交给 QObject 析构统一回收。
}

// ---------------------------------------------------------------------------
// 连接生命周期
// ---------------------------------------------------------------------------

void ClientConnection::connectTo(const QString &host, quint16 port, const QString &password,
                                 bool keepBackoff)
{
    teardown();
    handshakeReported_ = false;
    authRejected_      = false;   // 新的一次尝试：重新给「口令可能已被改对」的机会

    // 记下这次连接的参数（断线后重连要用 —— 口令以前只活在输入框里），
    // 并清掉「用户主动断开」标记；若已有重连在排队，这次连接直接把它顶掉。
    //
    // ⚠️ 只停定时器、**不无条件清零退避计数**：自动重连也走这条函数
    //（scheduleReconnect → onReconnectTimeout → connectTo），在这里清零等于
    // 每轮都把计数抹掉，退避恒为 1 倍基准（真机日志里 24 行全是「第 1 次」）。
    if (reconnectTimer_)
        reconnectTimer_->stop();
    if (!keepBackoff)
        reconnectAttempt_ = 0;

    userInitiatedDisconnect_ = false;
    lastHost_                = host;
    lastPort_                = port;
    lastPassword_            = password;

    // 心跳从「开始尝试连接」就跑起来 —— 如果 TCP 连上了但对端不回应握手（对端卡死），
    // 也要能被判死并继续退避重试，而不是永远挂在那儿。
    attemptTimer_.start();
    if (heartbeatTimer_)
        heartbeatTimer_->start();

    auto *socket   = new QTcpSocket(this);

    // 🔴 局域网直连工具**绝不走代理** —— 必须显式关掉。
    //
    // 缘起（真机部署时暴露的 bug）：`QTcpSocket` 默认的 proxy 是 `DefaultProxy`，
    // Qt 据此解析**系统代理**（Windows 的 Internet Settings，或环境变量 `*_proxy`）。
    // 后果很割裂：连 `127.0.0.1` 一切正常（本机地址通常在代理的 bypass 名单里），
    // 而连**局域网 IP** 会被代理拦下，报
    // RESULT DENY The proxy type is invalid for this operation
    // 现场表现就是「另一台机器明明跑着被控端、端口口令都对，就是连不上」——
    // 极易误判成网络/防火墙问题（第一反应都是去查防火墙）。
    // 用户关掉系统代理就好了，但这不该由用户承担。
    //
    // 设在这里而不是在 `main.cpp` 里做全局 `setApplicationProxy`：① 精确到「发起连接
    // 的那个 socket」，语义最清楚；② **可测**（见 tests/test_clientconnection.cpp）。
    // 重连走的是同一条路（onReconnectTimeout → connectTo），所以这一处就覆盖全部路径。
    socket->setProxy(QNetworkProxy::NoProxy);

    session_       = new Session(socket, this);
    authClient_    = new AuthClient(session_, password, this);

    // 会话一建好就通知上层刷按钮：`stateChanged` 让【断开】在
    // 「TCP 已连上但还在等握手」这段窗口期也可点（旧实现里 updateConnectionUi 会把
    // 未握手的会话也算进可用性）。
    emit stateChanged();

    connect(session_, &Session::frameReceived, this,
            [this](const Frame &frame) { handleFrame(frame); });

    connect(authClient_, &AuthClient::succeeded, this, [this, host, port]() {
        if (handshakeReported_)
            return;
        handshakeReported_ = true;
        ready_             = true;
        reconnectAttempt_  = 0;  // 连上了，退避计数清零
        ensureFileSender();      // 建好发送器并绑到这条 session
        emit logMessage(QStringLiteral("已连接到 %1:%2").arg(host).arg(port));

        // 连接成功后自动请求输入令牌（先连先得；抢不到点画面可重试）
        requestToken();

        emit readyChanged(true);
        emit stateChanged();
        emit handshakeFinished(true, QStringLiteral("connected to %1:%2").arg(host).arg(port));
    });

    connect(authClient_, &AuthClient::failed, this, [this]() {
        if (handshakeReported_)
            return;
        handshakeReported_ = true;
        ready_             = false;
        // 被拒不是网络故障：口令错重连只会再撞一次（每次都在被控端记一笔失败
        // 退避），满员重连在对方空出名额前也不会成功。标记下来让重连判定短路。
        authRejected_      = true;
        emit logMessage(QStringLiteral("对方拒绝了连接：口令不正确或对方连接数已满（已停止自动重连）"));
        emit statusMessage(QStringLiteral("对方拒绝连接：口令不正确或对方连接数已满"));
        emit stateChanged();
        emit handshakeFinished(false, QStringLiteral("denied"));
    });

    connect(socket, &QTcpSocket::connected, this, [this, host, port]() {
        emit logMessage(QStringLiteral("正在验证口令…"));
        if (authClient_)
            authClient_->start();
    });

    connect(socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!handshakeReported_) {
            handshakeReported_ = true;
            ready_             = false;
            const QString error = (session_ && session_->socket())
                                      ? session_->socket()->errorString()
                                      : QStringLiteral("socket error");
            emit logMessage(QStringLiteral("连接失败：%1").arg(error));
            emit statusMessage(QStringLiteral("连接失败"));
            emit stateChanged();
            emit handshakeFinished(false, error);
        }
        // 无论失败在握手前还是握手后（对端进程没了），都排一次重连；
        // 是不是真的要连由 scheduleReconnect 内部按开关 / 用户意图决定。
        scheduleReconnect();
    });

    // 对端断开（或把我们踢了）时复位界面，不要停留在「接收中」的假象里。
    connect(socket, &QTcpSocket::disconnected, this, [this]() { onSocketDisconnected(); });

    emit logMessage(QStringLiteral("正在连接 %1:%2 …").arg(host).arg(port));
    emit statusMessage(QStringLiteral("连接中…"));

    socket->connectToHost(host, port);
}

void ClientConnection::teardown()
{
    ready_             = false;
    handshakeReported_ = true;  // 本次尝试已结算，旧 socket 迟到的错误不再冒泡到界面

    // 心跳只属于「连接存续期间」。注意**不**在这里停 reconnectTimer_ ——
    // 断线清理紧接着就要靠它排下一次重连。
    if (heartbeatTimer_)
        heartbeatTimer_->stop();

    if (hasToken_) {
        hasToken_ = false;
        emit tokenChanged(false);
    }
    tokenRequested_ = false;  // 新会话重新申请，别被上一次的状态卡住

    // 发送器绑定在旧的 session 上，必须先中止并销毁
    if (fileSender_) {
        fileSender_->abort();
        delete fileSender_;
        fileSender_ = nullptr;
    }
    if (decoder_) {
        // 先停线程（quit + wait），再析构。wait() 不可省 ——
        // 解码线程里持有 MFT，且它的 COM 引用要在该线程内撤销。
        decoder_->stop();
        delete decoder_;
        decoder_ = nullptr;
    }
    decoderUnavailable_ = false;
    decoderOpenFailures_ = 0;
    lastDecoderRebuildMs_ = 0;   // 新连接不受上一次连接的重建间隔影响
    if (authClient_) {
        authClient_->deleteLater();
        authClient_ = nullptr;
    }
    if (session_) {
        session_->deleteLater();
        session_ = nullptr;
    }

    // 界面侧收口：按钮可用性 + 剪贴板目标（会话没了要退回被控端的令牌持有者）。
    emit stateChanged();
}

void ClientConnection::disconnectNow()
{
    userInitiatedDisconnect_ = true;
    cancelReconnect();  // 停退避 + 清零计数（用户点断开就得真的停，不能又连回来）

    // 优雅关闭：先让对端也收到断开（被控端据此释放令牌、停推画面）。
    if (session_ && session_->socket()
        && session_->socket()->state() != QAbstractSocket::UnconnectedState) {
        session_->socket()->disconnectFromHost();
    }

    const bool wasReady = ready_;
    teardown();
    emit connectionLost();
    if (wasReady)
        emit statusMessage(QStringLiteral("连接已断开"));
}

// 【重连】按钮。与「用户重新点一次【新连接…】」的区别只在于参数从哪来 ——
// 这里用**记下来的那一份**（`connectTo` 会自动 cancelReconnect + 清 userInitiatedDisconnect_ +
// 重置 attemptTimer_），所以不需要另写一套重试逻辑。
void ClientConnection::reconnectNow()
{
    if (lastHost_.isEmpty())
        return;   // 从没连过（新建的空对象），没有参数可用

    emit logMessage(QStringLiteral("手动重连 %1:%2 …").arg(lastHost_).arg(lastPort_));
    connectTo(lastHost_, lastPort_, lastPassword_);
}

// 断线统一收口：结算 → 拆内核 → 通知界面复位。
// 只在「确实有一条活连接在跑」时用于 onSocketDisconnected / 心跳判死；
// disconnectNow 走自己的分支（它要额外做优雅关闭与停退避）。
void ClientConnection::onSocketDisconnected()
{
    if (!handshakeReported_ && !ready_) {
        // 还没握手就断了：让命令行 / 界面知道这次连接没成
        handshakeReported_ = true;
        emit logMessage(QStringLiteral("连接已断开"));
        emit statusMessage(QStringLiteral("连接失败"));
        emit handshakeFinished(false, QStringLiteral("disconnected"));
    } else if (ready_) {
        emit logMessage(QStringLiteral("与对方的连接已断开"));
    }

    const bool wasReady = ready_;
    teardown();
    emit connectionLost();
    if (wasReady)
        emit statusMessage(QStringLiteral("连接已断开"));

    // 清理完之后再排重连（用户主动断开会在这里被抑制）
    scheduleReconnect();
}

// ---------------------------------------------------------------------------
// 令牌 / 输入 / 文件
// ---------------------------------------------------------------------------

void ClientConnection::requestToken()
{
    // 已有请求在途时不再发（幂等）：输入事件逐条来，未持令牌时每条都会走到这里，
    // 不设这个闩就会一帧一条 TokenReq 刷屏 —— 被控端每条都回一条拒绝日志，
    // 实测能被刷成一个按键几万行。对端解锁不会主动告知，所以保留「点一下重试」的入口。
    if (!session_ || !ready_ || hasToken_ || tokenRequested_)
        return;
    tokenRequested_ = true;
    session_->sendFrame(static_cast<std::uint8_t>(FrameType::TokenReq));
}

void ClientConnection::releaseToken()
{
    if (!hasToken_)
        return;
    hasToken_ = false;
    emit tokenChanged(false);
    if (session_ && ready_)
        session_->sendFrame(static_cast<std::uint8_t>(FrameType::TokenRelease));
    emit logMessage(QStringLiteral("已让出操作权，其他控制端可以接手"));
}

void ClientConnection::sendInput(const InputEvent &event, bool force)
{
    if (!session_ || !ready_)
        return;

    // `force` 的语义只有一条：**跳过键鼠总开关**（命令行 / 程序化自测用）。
    // 它**不**跳过令牌门控 —— 那是 4 控 1 的公平性依据，自测路径也不该绕开。
    if (!force && !inputEnabled_)
        return;

    // 释放类事件（KeyUp / 鼠标抬起）额外跳过**令牌门控**。
    //
    // 为什么必须这样：旧逻辑里未持令牌的事件会转成 `requestToken()` 然后**被丢弃**。
    // 对「抬起」来说这等于**永远补不上** —— 发起方以为自己已经松手，远端那个键
    // 却永久按下（按住 Ctrl 时对方整机基本不可用）。
    // 安全上无害：最坏只是让远端松掉一个本就没按下的键（不构成越权操作）。
    if (isReleaseEvent(event)) {
        session_->sendFrame(static_cast<std::uint8_t>(FrameType::InputEvent),
                            encodeInputEvent(event));
        emit inputSent(static_cast<int>(event.kind));
        return;
    }

    // 令牌门控：未持有时不发输入，先请求令牌。
    // 非鼠标移动的事件（按键 / 点按）也会走到这里，那是「用操作本身去申请令牌」的入口。
    if (!hasToken_) {
        requestToken();
        return;
    }

    session_->sendFrame(static_cast<std::uint8_t>(FrameType::InputEvent), encodeInputEvent(event));
    emit inputSent(static_cast<int>(event.kind));
}

void ClientConnection::ensureFileSender()
{
    if (fileSender_ || !session_)
        return;

    fileSender_ = new FileSender(session_, this);
    connect(fileSender_, &FileSender::started, this,
            [this](const QString &name, quint64 total) { emit fileStarted(name, total); });
    connect(fileSender_, &FileSender::progress, this,
            [this](quint64 sent, quint64 total) { emit fileProgress(sent, total); });
    connect(fileSender_, &FileSender::finished, this,
            [this](bool ok, const QString &path, const QString &message) {
                emit fileFinished(ok, path, message);
            });
}

bool ClientConnection::sendFile(const QString &path)
{
    if (!session_ || !ready_)
        return false;

    ensureFileSender();
    if (!fileSender_)
        return false;

    return fileSender_->start(path, nextTransferId_++);
}

// ---------------------------------------------------------------------------
// 收帧总入口
// ---------------------------------------------------------------------------

void ClientConnection::handleFrame(const Frame &frame)
{
    // 拆机会话用的是 deleteLater()，它真正销毁之前，socket 读缓冲里剩下的数据
    // 仍会被 Session 解析出来并补发 frameReceived。连接已经没了，这些迟到帧
    // 只会造成污染：TokenGrant 把 hasToken_ 翻成 true，ClipboardText 往本地
    // 剪贴板写字。所以先看会话在不在、这次连接还算不算数。
    if (!session_ || !ready_)
        return;

    const auto type = frame.type;

    // 心跳：只当作「对端还活着」的凭据 —— `Session` 收到任何字节都会刷新计时，
    // 这里什么都不用做。**不回帧**：两端各自都有定时器在发心跳，回一帧纯属多余；
    // 而两端都「收到就回」会形成无限乒乓，把轻量心跳变成帧风暴
    //（实测 42 s 收到 5.6 万帧；当时 socket 读还在 GUI 线程上，那一个线程的 CPU 从 35% 涨到 82%）。
    if (type == static_cast<std::uint8_t>(FrameType::Heartbeat))
        return;

    // 输入令牌
    if (type == static_cast<std::uint8_t>(FrameType::TokenGrant)) {
        tokenRequested_ = false;
        if (!hasToken_) {
            hasToken_ = true;
            emit tokenChanged(true);
            emit logMessage(QStringLiteral("已获得操作权，可以操作对方电脑"));
        }
        return;
    }
    if (type == static_cast<std::uint8_t>(FrameType::TokenRelease)) {
        // 被控端收回输入权 —— 机主点了【收回输入权】，或持有者空闲超时被自动让出。
        // 已知取舍：客户端分不清「被抢占」与「被锁定」，统一显示「等待中」。
        if (hasToken_) {
            hasToken_ = false;
            emit tokenChanged(false);
            emit logMessage(QStringLiteral("操作权已被对方收回（机主收回或空闲自动让出）"));
        }
        tokenRequested_ = false;  // 下一次输入事件会重新申请一次（主动收回后点一下即可拿回）
        return;
    }

    // 剪贴板（仅文本）—— 交给上层的同步器处理
    if (type == static_cast<std::uint8_t>(FrameType::ClipboardText)) {
        emit clipboardReceived(frame);
        return;
    }

    if (type == static_cast<std::uint8_t>(FrameType::ScreenFrameRaw)) {
        const QImage image = decodeRawFrame(frame.payload);
        if (image.isNull())
            return;

        ++rxFrames_;
        rxBytes_ += static_cast<quint64>(frame.payload.size());
        // 裸帧路径没有解码器，图就是原尺寸 —— 真尺寸 = 图尺寸
        emit frameReady(image, static_cast<qint64>(frame.payload.size()), image.size());
        return;
    }

    if (type != static_cast<std::uint8_t>(FrameType::ScreenFrameH264))
        return;
    if (decoderUnavailable_)
        return;

    const auto h264 = decodeH264Frame(frame.payload);
    if (!h264)
        return;

    // 解码器曾开失败过：只在**关键帧**上重试。理由 ——
    // · 一次性闩锁会让瞬态失败变成永久黑屏（系统临时没资源、别的程序占着解码器）；
    // · 但每帧都重试会白烧 CPU（每帧建一次注定失败的 MFT）。
    // 关键帧自带 SPS/PPS，是重建解码器最自然的时机。
    if (!decoder_ && decoderOpenFailures_ > 0 && !h264->keyframe)
        return;

    const int frameW = static_cast<int>(h264->width);
    const int frameH = static_cast<int>(h264->height);

    // 分辨率变了或首次收到 H.264 时（重）建解码线程。
    // 解码跑在专属线程里 —— 那个线程自己也拿着一个已 open 的 MFT，
    // 尺寸变化只能靠「停掉旧线程 + 起新线程」来切换（同步 MFT 不能跨线程搬运）。
    const bool sizeMismatch = decoder_ && (decoder_->width() != frameW || decoder_->height() != frameH);

    if (sizeMismatch) {
        // ⚠️ 尺寸来自**对端数据**，可以被逐帧翻转；而重建是同步重操作（停 + 删 + 起 +
        // 等 MFT 就绪，全在 UI 线程，几十毫秒）。逐帧重建＝对端一个廉价 DoS。
        // 所以不立刻重建：只在**关键帧**上应用新尺寸（关键帧自带 SPS/PPS，本就该开启
        // 一个新序列），并叠一个**最小重建间隔**兜底 —— 关键帧标记同样来自线上，
        // 只靠它挡不住「每帧都标关键帧」。
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool   apply = h264->keyframe
                           && (nowMs - lastDecoderRebuildMs_) >= decoderRebuildIntervalMs_;
        if (!apply)
            return;   // 尺寸与本机解码器不符的这一帧：解码只会得到尺寸错乱的图，直接丢
    }

    if (decoder_ && sizeMismatch) {
        decoder_->stop();
        delete decoder_;
        decoder_ = nullptr;
    }
    if (!decoder_) {
        auto *worker = new DecoderWorker(frameW, frameH, this);
        // 新（重建）的解码线程要接上当前的显示预算。分辨率变化时会走到这里
        // （上面把旧 worker 停掉删掉了），所以不必在别处补。
        worker->setOutputSize(displayBudget_);

        // 产出：把图交给上层渲染。QImage 隐式共享，跨线程按值传递安全
        // （深拷贝只在真被改写时才发生）。这个槽在 UI 线程执行（worker 的 QObject
        // 亲和性在创建它的线程），恰好就是渲染要的地方。
        //
        // 字节数按**本次投递的那一帧**算：解码异步，一次投递可能产出 0..N 张，
        // 沿用 「每产出一张就加一次该帧 payload 字节」的口径（统计口径不变）。
        connect(worker, &DecoderWorker::frameDecoded, this,
                [this](const QImage &image, const QSize &sourceSize) {
                    ++rxFrames_;
                    rxBytes_ += static_cast<quint64>(pendingFrameBytes_);
                    emit frameReady(image, pendingFrameBytes_, sourceSize);
                });
        connect(worker, &DecoderWorker::decodeFailed, this, [this](const QString &message) {
            emit logMessage(QStringLiteral("画面解码失败：%1").arg(message));
        });

        worker->start();
        if (!worker->waitReady()) {
            // 首帧同步等一次：开 MFT 最多几十毫秒，不值得为它把收帧做成异步状态机。
            emit logMessage(QStringLiteral("画面解码器启动失败（第 %1 次）：%2")
                                .arg(decoderOpenFailures_ + 1)
                                .arg(worker->lastError()));
            worker->stop();
            delete worker;
            // 连续失败到上限才判「本机不可用」，此前允许在后续关键帧上再试。
            if (++decoderOpenFailures_ >= kMaxDecoderOpenFailures) {
                decoderUnavailable_ = true;
                emit logMessage(QStringLiteral("本机无法解码画面，后续画面将无法显示"));
            }
            return;
        }
        decoderOpenFailures_ = 0;   // 成功即清零
        decoder_ = worker;
        lastDecoderRebuildMs_ = QDateTime::currentMSecsSinceEpoch();  // 节流的时间基准
        emit logMessage(QStringLiteral("画面解码就绪（%1）").arg(decoder_->decoderName()));
    }

    // 本帧的字节数记下来供 frameDecoded 回调使用（见上面的口径说明）。
    pendingFrameBytes_ = static_cast<qint64>(frame.payload.size());

    // 投递一帧。`decode()` 内部是**线程安全入队**（互斥量 + 条件变量），
    // 解码在那个专属线程里跑 —— 这里只是把 QByteArray 压进队列（隐式共享，不深拷贝）。
    //
    // ⚠️ 别改成 `QMetaObject::invokeMethod(decoder_, "decode", Qt::QueuedConnection, …)`：
    // DecoderWorker 是 QThread 子类，它的 QObject 亲和性在**创建它的线程**（这里 = UI 线程），
    // 那个队列事件会投到 UI 线程的事件循环里 —— 解码又回到 UI 线程，且静默生效。
    // DecoderWorker 刻意用显式队列就是为了把这条路堵死（详见其头文件注释）。
    decoder_->decode(h264->bitstream, h264->keyframe);
}

// ---------------------------------------------------------------------------
// 心跳 + 断线自动重连（控制端侧）
// ---------------------------------------------------------------------------

bool ClientConnection::reconnectPending() const
{
    return reconnectTimer_ && reconnectTimer_->isActive();
}

void ClientConnection::cancelReconnect()
{
    if (reconnectTimer_)
        reconnectTimer_->stop();
    reconnectAttempt_ = 0;
}

// 显示预算（窗口中央区尺寸）-> 解码输出尺寸上限框。
// 下发走 `DecoderWorker::setOutputSize()` —— 它内部是**线程安全入队**，
// 不可以改用 `invokeMethod` 投递（那会投到本条连接所在的 UI 线程）。
// 解码器是惰性创建的，所以在它还不存在时只记值；存在则立刻下发（窗口 resize 走这条）。
void ClientConnection::setDisplayBudget(const QSize &size)
{
    if (displayBudget_ == size)
        return;
    displayBudget_ = size;
    if (decoder_)
        decoder_->setOutputSize(size);
}

// 死链判定：对端静默超过了阈值。静默的计时起点分两种 —— 见头文件说明。
bool ClientConnection::isDeadLink(bool hasReceived, qint64 msSinceLastRx,
                                  qint64 attemptElapsedMs, qint64 deadTimeoutMs)
{
    return hasReceived ? (msSinceLastRx > deadTimeoutMs)
                       : (attemptElapsedMs > deadTimeoutMs);
}

// 3 s 一次：发一帧心跳（让对方知道我们还在），并检查对方是否还在。
void ClientConnection::onHeartbeatTick()
{
    if (!session_)
        return;

    QTcpSocket *sock = session_->socket();
    if (!sock || sock->state() != QAbstractSocket::ConnectedState)
        return;  // 没连上（正在退避 / 已断开）就没什么可发、也没什么可判

    session_->sendFrame(static_cast<std::uint8_t>(FrameType::Heartbeat));

    // 判死口径（都要「静默」满 10 s）：
    // ① 收过字节：10 s 没收到对端任何数据 —— Wi-Fi 掉了 / 对端进程被强杀；
    // ② 一帧未收（重连中，TCP 连上了却等不到握手响应）—— 对端卡死在半开连接上。
    // ② 的计时起点是「本次尝试开始」，不能拿 msSinceLastRx() 去比：那时它还是
    // 「从未收到」哨兵，会让刚连上的连接在**首个** tick（3 s）就被判死。
    if (!isDeadLink(session_->hasReceived(), session_->msSinceLastRx(),
                    attemptTimer_.elapsed(), kClientDeadTimeoutMs))
        return;

    emit logMessage(ready_
                        ? QStringLiteral("与被控端失联超过 %1 秒，主动断开连接")
                              .arg(kClientDeadTimeoutMs / 1000)
                        : QStringLiteral("重连后 %1 秒内没等到握手响应，判定对端无响应")
                              .arg(kClientDeadTimeoutMs / 1000));

    // 用 abort() 而不是 disconnectFromHost()：对方已经死了/卡住了，等它回 FIN
    // 只会把我们卡在 ClosingState —— 既不发 disconnected，也重连不了。
    sock->abort();

    const bool wasReady = ready_;
    teardown();
    emit connectionLost();
    if (wasReady)
        emit statusMessage(QStringLiteral("连接已断开"));

    scheduleReconnect();
}

// 排一次重连：退避 1 → 2 → 4 → 8 → 16 → 30 s 封顶，成功后由握手回调清零。
// 幂等 —— 一次断线可能同时触发 errorOccurred 与 disconnected，只排一次。
void ClientConnection::scheduleReconnect()
{
    if (userInitiatedDisconnect_ || !autoReconnect_ || authRejected_ || lastHost_.isEmpty())
        return;
    if (reconnectPending())
        return;

    const int shift   = std::min(reconnectAttempt_, 5);  // ×1,2,4,8,16,32
    const int delayMs = std::min(reconnectBaseDelayMs_ << shift, kReconnectMaxDelayMs);
    ++reconnectAttempt_;

    reconnectTimer_->start(delayMs);
    emit stateChanged();  // 重连待命期间把【断开】放出来（用户的中止入口）

    const QString msg = QStringLiteral("连接已断开，%1 秒后重连…（第 %2 次）")
                            .arg(delayMs / 1000)
                            .arg(reconnectAttempt_);
    emit logMessage(msg);
    emit statusMessage(msg);
}

void ClientConnection::onReconnectTimeout()
{
    // authRejected_ 也在这里挡一道：定时器可能在拒绝到达之前就排上了，
    // 光靠 scheduleReconnect 的早退不够。
    if (userInitiatedDisconnect_ || !autoReconnect_ || authRejected_)
        return;

    emit logMessage(QStringLiteral("正在重连 %1:%2 …（第 %3 次）")
                        .arg(lastHost_)
                        .arg(lastPort_)
                        .arg(reconnectAttempt_));
    // keepBackoff=true：**保留**退避计数，让下一轮的延迟真的往上涨
    //（connectTo 只在「用户发起」时清零；握手成功后也会归零）。
    connectTo(lastHost_, lastPort_, lastPassword_, /*keepBackoff=*/true);
}

} // namespace lanrdp
