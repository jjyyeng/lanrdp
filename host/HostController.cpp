#include "host/HostController.h"

#include "app/LogPort.h"
#include "host/InputInjector.h"
#include "host/ScreenStreamer.h"
#include "net/FileReceiver.h"
#include "net/HostServer.h"
#include "net/Session.h"
#include "protocol/InputEvent.h"

#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace lanrdp {

namespace {

// 空闲让出的兜底判定粒度（与从前 MainWindow 的 5 s 一致）。
constexpr int kIdleCheckIntervalMs = 5000;
// 统计推给 UI 的周期。GUI 不能按需来读（跨线程），只能由本线程主动送。
constexpr int kStatsIntervalMs = 1000;

// 「ip:port」形式的对端标识（日志与列表都用它）。
QString peerName(const Session *s)
{
    if (!s || !s->socket())
        return QStringLiteral("?");
    return QStringLiteral("%1:%2")
        .arg(s->socket()->peerAddress().toString())
        .arg(s->socket()->peerPort());
}

// 毫秒写成人话。整分钟才用「分钟」，否则用「秒」——
// 这样验收时把超时常量临时改小（如 12 s）日志也不会自相矛盾。
QString humanDuration(qint64 ms)
{
    const qint64 secs = ms / 1000;
    if (secs >= 60 && secs % 60 == 0)
        return QStringLiteral("%1 分钟").arg(secs / 60);
    return QStringLiteral("%1 秒").arg(secs);
}

} // namespace

HostController::HostController(LogPort *logPort, QObject *parent)
    : QObject(parent)
    , logPort_(logPort)
{
    // ⚠️ 这里**故意不建任何 QTimer** —— 全部推迟到 start()。
    //
    // `QTimer` 的线程亲和性在**构造时**确定，而本对象会被 `moveToThread` 搬到
    // 工作线程。把「创建」集中到 start()（在工作线程里执行）
    // ⇒ 亲和性只有**一个决定点**，而不是一半靠 `moveToThread` 的隐式子对象语义；
    // 顺带让「构造后没有 QTimer」成为一条可断言的不变量
    //（tests/test_hostcontroller.cpp::timersExistOnlyAfterStart）。
    //
    // ⚠️ 代价：对象一旦有 parent 就完全不能 `moveToThread` —— 创建方必须传 nullptr。
}

// 幂等：在**当前线程**里建立内部定时器。
//
// 只从 start() 调用 —— 那一刻本对象已经在它真正归属的线程上了。
void HostController::ensureTimers()
{
    // 空闲让出的兜底判定（5 s）：只在监听期间跑（起停在 start/stop 里控制）。
    // 没人来请求令牌时状态不会自己更新，只有它能收回「人走了但连接还活着」的令牌。
    if (!idleTimer_) {
        idleTimer_ = new QTimer(this);
        idleTimer_->setInterval(kIdleCheckIntervalMs);
        connect(idleTimer_, &QTimer::timeout, this, &HostController::onIdleCheck);
    }

    // 统计：每秒推一次快照。菜单 / 列表那些是**事件驱动**的（notifyUi 在变化处），
    // 但「帧率 / 带宽」这类计数是连续变化的，等不到事件 —— 只能定时送。
    if (!statsTimer_) {
        statsTimer_ = new QTimer(this);
        statsTimer_->setInterval(kStatsIntervalMs);
        connect(statsTimer_, &QTimer::timeout, this, [this] { notifyUi(); });
        statsTimer_->start();
    }
}

HostController::~HostController()
{
    stop();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool HostController::start(quint16 port, const QString &password)
{
    // 重复调用（或先「停止监听」再启动）时，先干净地拆掉上一轮，避免残留野 target。
    if (host_)
        stop();

    // 内部定时器与看门狗线程在这里才建立 —— 见构造函数里的说明。
    ensureTimers();

    if (!streamer_) {
        streamer_ = new ScreenStreamer(this);

        // 本机预览：关着就**根本不发**。
        // 跨线程投递下这不只是省事 —— GUI 若被模态循环占住，队列会积压（4K 每帧
        // 独立缓冲 ≈33 MB）⇒ 必须由**本侧**过滤，而不是「发了让 GUI 丢掉」。
        connect(streamer_, &ScreenStreamer::frameCaptured, this, [this](const QImage &frame) {
            if (previewEnabled_)
                emit previewFrame(frame);
            // 帧尺寸无条件上报：它是状态栏的分辨率口径，与本机预览开关无关。
            emit frameSizeChanged(frame.size());
        });

        connect(streamer_, &ScreenStreamer::failed, this, [this](const QString &error) {
            emit logLine(QStringLiteral("抓屏失败，已停止推送：%1").arg(error));
            emit statusText(QStringLiteral("画面采集失败"));
            emit captureFailed(error);
        });

        // 抓屏**暂时**失效（UAC 安全桌面 / 锁屏 / 分辨率变化…）会自愈、不停流。
        // 这里只做可见性提示，让人知道「不是死了，是在重试」。
        connect(streamer_, &ScreenStreamer::captureInterrupted, this, [this](const QString &reason) {
            emit logLine(QStringLiteral("画面采集中断，正在自动重试：%1").arg(reason));
            emit statusText(QStringLiteral("画面中断，重试中…"));
        });

        connect(streamer_, &ScreenStreamer::captureResumed, this, [this]() {
            emit logLine(QStringLiteral("画面采集已恢复"));
        });

        connect(streamer_, &ScreenStreamer::codecFallback, this, [this](const QString &reason) {
            emit logLine(QStringLiteral("H.264 编码不可用，已退回裸帧：%1").arg(reason));
        });

        // 编码器能力受限（目前 = 没有 ICodecAPI）。非致命，但必须让人看见 ——
        // 否则「码率设了没效果」「新观看者要等好几秒才出画」都会成为无从解释的现象。
        connect(streamer_, &ScreenStreamer::encoderNote, this, [this](const QString &note) {
            emit logLine(QStringLiteral("编码器说明：%1").arg(note));
        });

        // 某个控制端接收太慢、积压持续超上限 ⇒ 已被停止推帧。这里把连接断开，
        // 既释放名额，也让对端明确知道「被踢了」而不是「画面卡住」。
        connect(streamer_, &ScreenStreamer::targetCongested, this,
                [this](Session *session, const QString &reason) {
                    emit logLine(QStringLiteral("接收端过慢，已断开该连接：%1").arg(reason));
                    emit statusText(QStringLiteral("已断开一个网络过慢的控制端"));
                    if (session && session->socket())
                        session->socket()->disconnectFromHost();
                });
    }

    // 应用当前的编码选择（无论 streamer_ 是新建还是复用）。
    applyCodec();

    host_ = new HostServer(password, this);

    connect(host_, &HostServer::listening, this, &HostController::listening);

    // 监听失败：把可行动的原因记进日志并原样转发（`--host` 无界面模式靠它
    // 打 stderr 并以非 0 退出，而不是默默挂着让人以为服务已经起来了）。
    connect(host_, &HostServer::listenFailed, this, [this](quint16 failedPort, const QString &reason) {
        emit logLine(reason);
        emit listenFailed(failedPort, reason);
    });

    connect(host_, &HostServer::clientAuthenticated, this, &HostController::handleAuthenticated);
    connect(host_, &HostServer::clientRejected, this, &HostController::handleRejected);
    connect(host_, &HostServer::clientDisconnected, this, &HostController::handleDisconnected);

    if (!host_->listen(port)) {
        // 具体原因已由 listenFailed 那一路给出，这里只回「起不来」。
        return false;
    }

    if (idleTimer_)
        idleTimer_->start();   // 空闲让出的兜底判定只在监听期间跑

    emit logLine(QStringLiteral("正在监听 0.0.0.0:%1").arg(host_->port()));
    emit statusText(QStringLiteral("监听中：端口 %1").arg(host_->port()));
    notifyUi();   // 状态变了 —— 让 UI 那份快照立刻跟上
    return true;
}

// 兜底：我们注入过「按下」、却还没注入过「抬起」时，替对端补一针松开。
//
// 场景：网络正好在按下与抬起之间断掉 / 控制端被踢 / 机主收回输入权 / 停止监听 ——
// 那一针「抬起」再也送不到，被控端的鼠标键就会一直停在按下态（表现为拖拽不松手）。
// ⚠️ 这不是靠计时猜：`mouseButtonsDown_` 只在我们**确实注入成功**过按下时才非 0，
// 且收到抬起就清位 —— 所以正常操作永远不会被误伤。
void HostController::releaseStuckMouseButtons(const QString &reason)
{
    if (mouseButtonsDown_ == 0)
        return;

    const bool ok = lanrdp::releaseAllMouseButtons();
    mouseButtonsDown_ = 0;
    emit logLine(ok ? QStringLiteral("⚠ %1：补发了一针鼠标「抬起」（否则被控端会一直按着不放）")
                          .arg(reason)
                    : QStringLiteral("⚠ %1：想补发鼠标「抬起」，但被系统拒绝（目标窗口权限更高？）")
                          .arg(reason));
}

void HostController::stop()
{
    // 先补掉悬空的按下：停监听之后，对端就算想送「抬起」也没路了。
    releaseStuckMouseButtons(QStringLiteral("停止监听"));

    if (!host_)
        return;

    // 必须先把与会话绑定的东西逐个摘掉：host_ 是这些 Session 的父对象，
    // 一旦销毁，clientDisconnected 就没机会再发出来，streamer 会残留野 target。
    const QVector<Session *> sessions = host_->sessions();
    for (Session *s : sessions) {
        tokenGate_.remove(tokenOf(s));
        if (streamer_)
            streamer_->removeTarget(s);
    }
    // 全部会话都要走了：身份表整体清空（被控端是唯一使用 tokenGate_ 的角色）。
    tokenIds_.clear();

    // 停止监听一并解锁，否则会留下「一个控制端都看不见、输入却被锁着」的死状态。
    tokenGate_.unlock();
    if (idleTimer_)
        idleTimer_->stop();

    if (sessions.isEmpty() && streamer_)
        streamer_->clearTargets();
    if (streamer_)
        streamer_->stop();

    if (fileReceiver_) {   // 半截文件不能留
        fileReceiver_->abort();
        delete fileReceiver_;
        fileReceiver_ = nullptr;
    }

    host_->stop();
    host_->deleteLater();   // 连同其 Session / 监听套接字一起销毁 -> 对端收到断开
    host_ = nullptr;

    emit logLine(QStringLiteral("已停止监听（断开 %1 个控制端）").arg(sessions.size()));
    emit statusText(QStringLiteral("已停止监听"));
    notifyUi();
}

bool HostController::isListening() const
{
    return host_ && host_->isListening();
}

quint16 HostController::port() const
{
    return host_ ? host_->port() : 0;
}

// ---------------------------------------------------------------------------
// 会话事件
// ---------------------------------------------------------------------------

void HostController::handleAuthenticated(Session *session)
{
    const QString peer = peerName(session);
    emit logLine(QStringLiteral("控制端已通过鉴权：%1").arg(peer));

    // ---- 远端文件 -> 落盘 ----
    ensureFileReceiver();

    if (session) {
        // ---- 远端输入 / 令牌 / 剪贴板 / 文件，全部从这一条出口分流 ----
        connect(session, &Session::frameReceived, this,
                [this, session](const Frame &frame) { handleSessionFrame(session, frame); });
    }

    if (streamer_) {
        // 「抓屏源」与「编码」是全局信息（整台机器一份），只在**第一个**目标接入时
        // 记一次：每个控制端都刷一遍的话，4 个控制端连进来就是同一段话刷 4 遍，
        // 后面的连接记录会被这几行挤走。
        const bool firstTarget = streamer_->targetCount() == 0;

        streamer_->addTarget(session);
        streamer_->start();

        if (firstTarget) {
            const QString info = streamer_->sourceInfo();
            if (!info.isEmpty())
                emit logLine(QStringLiteral("抓屏源：%1").arg(info));

            if (streamer_->codec() == StreamCodec::H264) {
                const QString encoder = streamer_->codecInfo();
                emit logLine(encoder.isEmpty()
                                 ? QStringLiteral("编码：H.264（编码器将在首帧时打开）")
                                 : QStringLiteral("编码：H.264 / %1").arg(encoder));
            } else {
                emit logLine(QStringLiteral("编码：裸帧 BGRA（未压缩）"));
            }
        }
    }

    notifyUi();
    emit statusText(QStringLiteral("控制端已连接，正在推送画面"));
}

void HostController::handleRejected(Session *session)
{
    // 被拒的会话也会走「创建 -> 销毁」这一遭：顺手清掉它的身份记录，
    // 免得地址被下一个会话复用时查到这条陈旧映射。
    if (session)
        tokenIds_.erase(session);

    const int count = host_ ? host_->sessions().size() : 0;
    const int max   = host_ ? host_->maxClients() : 0;
    if (host_ && count >= max) {
        emit logLine(QStringLiteral("有连接被拒绝：控制端数量已达上限（%1）").arg(max));
        emit statusText(QStringLiteral("已拒绝：控制端已满"));
    } else {
        emit logLine(QStringLiteral("有连接因口令错误被拒绝 (DENY)"));
        emit statusText(QStringLiteral("已拒绝：口令错误"));
    }
}

void HostController::handleDisconnected(Session *session)
{
    emit logLine(QStringLiteral("控制端断开"));

    // 若断开的是令牌持有者，令牌立即空出，供其他控制端请求。
    if (session && tokenGate_.holds(tokenOf(session))) {
        tokenGate_.remove(tokenOf(session));
        // 它的「抬起」再也送不到了 —— 有悬空的按下就补一针松开。
        releaseStuckMouseButtons(QStringLiteral("操作权持有者断开"));
        emit logLine(QStringLiteral("操作权持有者断开，操作权已释放"));
    }
    // 身份表也必须同步清掉：留着的话，新会话若落在同一地址会查到旧 id。
    if (session)
        tokenIds_.erase(session);
    notifyUi();

    // 半截文件不能留：对端可能正好传到一半掉线。
    // ⚠️ 只收掉**断开这条连接自己那一份** —— 接收器按来源隔离，别连累别人。
    if (fileReceiver_)
        fileReceiver_->abort(session);

    if (streamer_) {
        streamer_->removeTarget(session);
        if (streamer_->targetCount() == 0) {
            streamer_->stop();
            emit statusText(QStringLiteral("控制端已断开，暂停推送"));
            return;
        }
    }
    emit statusText(QStringLiteral("控制端已断开"));
}

void HostController::handleSessionFrame(Session *session, const Frame &frame)
{
    // ---- 心跳：只用来确认「对端还活着」，不写日志、**不回帧** ----
    // 发送侧由 HostServer 的统一定时器负责（3 s 一次），回帧纯属多余；
    // 两端都「收到就回」会形成无限乒乓，把轻量心跳变成帧风暴。
    if (frame.type == static_cast<std::uint8_t>(FrameType::Heartbeat))
        return;

    // ---- 输入令牌帧 ----
    if (frame.type == static_cast<std::uint8_t>(FrameType::TokenReq)) {
        // 惰性过期可能正好在这一刻让出令牌 —— 被让出的那位必须和「定时器那条路」
        // 一样收到 TokenRelease，否则他界面还显示握着操作权、输入却被一律静默拒绝。
        TokenGate::Token expired = 0;
        const bool granted = tokenGate_.request(tokenOf(session), &expired);
        if (expired != 0)
            announceIdleRelease(expired);

        if (granted) {
            session->sendFrame(static_cast<std::uint8_t>(FrameType::TokenGrant));
            emit logLine(QStringLiteral("已授予操作权：%1").arg(peerName(session)));
        } else {
            // 拒绝时**回一帧 TokenRelease** 当作「明确答复」。
            // 光拒绝不回答的话，控制端那头分不清「在途」还是「被拒」，
            // 只能一遍遍重问 —— 实测一次按键能刷出两万多行拒绝日志。
            session->sendFrame(static_cast<std::uint8_t>(FrameType::TokenRelease));
            if (tokenGate_.isLocked())
                emit logLine(QStringLiteral("操作权请求被拒（已锁定）：%1").arg(peerName(session)));
            else
                emit logLine(
                    QStringLiteral("操作权请求被拒（已有其他控制端持有）：%1").arg(peerName(session)));
        }
        notifyUi();
        return;
    }
    if (frame.type == static_cast<std::uint8_t>(FrameType::TokenRelease)) {
        tokenGate_.release(tokenOf(session));
        // 令牌交出去了，后续按键不再从这条连接来 —— 悬空的按下要就地补掉。
        releaseStuckMouseButtons(QStringLiteral("控制端让出操作权"));
        emit logLine(QStringLiteral("控制端让出了操作权：%1").arg(peerName(session)));
        notifyUi();
        return;
    }

    // 剪贴板 —— 只认「持有输入令牌」的那个控制端发来的内容。
    // 少了这道门，任何连上的控制端都能往机主剪贴板里塞东西。
    if (frame.type == static_cast<std::uint8_t>(FrameType::ClipboardText)) {
        if (tokenGate_.holds(tokenOf(session)))
            emit clipboardFrameReceived(frame);
        return;
    }

    // FILE_* 三帧交给接收器；不是文件帧时它会返回 false。
    // 与剪贴板同级受令牌门控 —— 文件落盘同样是对本机的写操作。
    if (fileReceiver_ && tokenGate_.holds(tokenOf(session)) && fileReceiver_->onFrame(session, frame))
        return;

    if (frame.type != static_cast<std::uint8_t>(FrameType::InputEvent))
        return;

    // 令牌门控：只有持有令牌的控制端才能注入（4 控 1 仅 1 可输入）。
    if (!tokenGate_.allowInput(tokenOf(session)))
        return;

    const auto input = decodeInputEvent(frame.payload);
    if (!input)
        return;

    // 记一次「真实活动」，作为空闲自动让出的基准。
    // 纯鼠标移动**不算** —— 控制端的 MouseMove 是持续上报的，
    // 鼠标停在画面区轻微抖动就会不断续期，5 分钟永远到不了。
    if (input->kind != InputKind::MouseMove)
        tokenGate_.touch(tokenOf(session));

    const bool ok = lanrdp::injectEvent(*input, targetMonitorRect(), virtualDesktopRect());

    if (ok) {
        // 只记**注入成功**的鼠标按键：没进系统的那一针，系统那边本来就没按下，不用我们去松。
        mouseButtonsDown_ = lanrdp::updatePressedMask(mouseButtonsDown_, *input);
        ++injectedCount_;
        emit inputInjected(static_cast<int>(input->kind));
    } else {
        ++injectFailedCount_;
        if (injectFailedCount_ == 1) {
            // 常见原因：被操作的是管理员权限窗口，普通权限进程无法注入（UIPI）。
            emit logLine(
                QStringLiteral("输入注入被系统拒绝（目标窗口权限更高？以管理员身份运行可解决）"));
        }
    }
}

// ---------------------------------------------------------------------------
// 客户端列表 / 令牌
// ---------------------------------------------------------------------------

HostController::UiState HostController::uiState() const
{
    UiState s;
    s.listening   = isListening();
    s.inputLocked = tokenGate_.isLocked();
    s.clientCount = clientCount();
    s.clients     = clients();
    s.framesSent        = framesSent();
    s.bytesSent         = bytesSent();
    s.injectedCount     = injectedCount_;
    s.injectFailedCount = injectFailedCount_;
    return s;
}

// 把当前快照推给 UI。跨线程连接会自动排队 ⇒ 这里就是「非阻塞地通知一次」。
// 凡是会改变状态 / 列表 / 统计的路径，收尾都要调一次。
void HostController::notifyUi()
{
    emit uiStateChanged(uiState());
}

// 把一段已编码好的剪贴板帧发给当前持令牌的控制端（无人持令牌就丢弃）。
// UI 侧不能自己干这件事：它既拿不到可靠的持有者指针，也不能碰本线程的 socket。
void HostController::sendClipboardFrame(const Frame &frame)
{
    if (Session *holder = holderSession())
        holder->sendFrame(frame);
}

QVector<HostController::ClientInfo> HostController::clients() const
{
    QVector<ClientInfo> out;
    if (!host_)
        return out;

    const QVector<Session *> sessions = host_->sessions();
    out.reserve(sessions.size());
    for (Session *s : sessions) {
        ClientInfo info;
        info.sessionId = s->id();
        info.peer      = peerName(s);
        // ⚠️ 这里**不能**走 tokenOf()：它是惰性分配的写操作，会改动身份表 ——
        // 而本函数是个「读快照」的查询，顺手改状态既违反直觉，也让本函数无法是 const。
        info.isHolder = isHolderOf(s);
        out.push_back(info);
    }
    return out;
}

int HostController::clientCount() const
{
    return host_ ? host_->sessions().size() : 0;
}

void HostController::dropClient(quint64 sessionId)
{
    if (host_)
        host_->dropClient(sessionId);   // 走 clientDisconnected -> 刷新 + 释放令牌
}

void HostController::dropAllClients()
{
    if (host_)
        host_->dropAllClients();
}

bool HostController::inputLocked() const
{
    return tokenGate_.isLocked();
}

Session *HostController::holderSession() const
{
    return sessionOfToken(tokenGate_.holder());
}

// 收回远程输入权。
// 三段式顺序不能反：① 先上锁 ② 再摘掉持有者 ③ 最后通知对端。
// 反过来（先释放）会留一个「刚释放 -> 对方 request -> 又拿到」的竞态窗口，
// 功能会时灵时不灵 —— 这正是「只 release 不 lock」不能用的原因。
void HostController::takeInput()
{
    tokenGate_.lock();

    Session *holder = holderSession();
    tokenGate_.release(tokenOf(holder));
    // 收回时对方可能正按着鼠标 —— 补一针松开，别把被控端卡在按下态。
    releaseStuckMouseButtons(QStringLiteral("收回操作权"));
    if (holder)
        holder->sendFrame(static_cast<std::uint8_t>(FrameType::TokenRelease));

    if (holder)
        emit logLine(QStringLiteral("已收回操作权并锁定（原持有者：%1）").arg(peerName(holder)));
    else
        emit logLine(QStringLiteral("已锁定（当前没有控制端持有操作权）"));
    emit statusText(QStringLiteral("已锁定：控制端暂时无法操作本机"));

    notifyUi();
}

void HostController::unlockInput()
{
    tokenGate_.unlock();
    emit logLine(QStringLiteral("已恢复，控制端可以再次申请操作本机"));
    emit statusText(QStringLiteral("已恢复：控制端可以重新申请操作"));
    notifyUi();
}

// 空闲让出后的善后：通知本人 + 补发松开 + 记一行日志。
// 两条触发路径（低频定时器 / 有人来抢时的惰性判定）共用这一份。
void HostController::announceIdleRelease(TokenGate::Token expired)
{
    // expireIfIdle() 已经把持有者清空了，所以这里用返回的**身份**反查会话 ——
    // 不能再去问 holderSession()（那时它只会返回 nullptr）。
    Session *idle = sessionOfToken(expired);
    if (idle)
        idle->sendFrame(static_cast<std::uint8_t>(FrameType::TokenRelease));

    // 人走了但可能还按着鼠标（最后一条事件是「按下」）—— 补一针松开。
    releaseStuckMouseButtons(QStringLiteral("空闲自动让出操作权"));

    emit logLine(QStringLiteral("控制端 %1 已空闲 %2（纯鼠标移动不算操作），自动让出操作权")
                     .arg(idle ? peerName(idle) : QStringLiteral("（已断开）"),
                          humanDuration(TokenGate::kIdleTimeoutMs)));
}

// 没人来请求时状态不会自己更新，只有这个低频定时器才能把
// 「持有者人走了、但 TCP 连接还活着」的令牌收回来并刷新列表显示。
void HostController::onIdleCheck()
{
    const TokenGate::Token expired = tokenGate_.expireIfIdle();
    if (expired == 0)
        return;

    announceIdleRelease(expired);
    notifyUi();
}

TokenGate::Token HostController::tokenOf(Session *s)
{
    if (!s)
        return 0;   // 0 = 无效身份（TokenGate 一律拒）

    const auto it = tokenIds_.find(s);
    if (it != tokenIds_.end())
        return it->second;

    const TokenGate::Token id = tokenGate_.issue();
    tokenIds_.emplace(s, id);
    return id;
}

// 「这个会话此刻是不是令牌持有者」——纯读，不改身份表（见 clients() 的说明）。
bool HostController::isHolderOf(Session *s) const
{
    if (!s)
        return false;
    const TokenGate::Token holder = tokenGate_.holder();
    if (holder == 0)
        return false;
    const auto it = tokenIds_.find(s);
    return it != tokenIds_.end() && it->second == holder;
}

Session *HostController::sessionOfToken(TokenGate::Token t) const
{
    if (t == 0)
        return nullptr;
    for (const auto &pair : tokenIds_) {
        if (pair.second == t)
            return pair.first;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 编码 / 文件 / 统计
// ---------------------------------------------------------------------------

void HostController::setCodec(bool useH264, int bitrateBps)
{
    useH264_ = useH264;
    bitrate_ = bitrateBps;
    applyCodec();
}

void HostController::applyCodec()
{
    if (!streamer_)
        return;

    // 编码帧率跟着抓屏间隔走。
    const int fps = qMax(1, 1000 / qMax(1, streamer_->intervalMs()));
    streamer_->setH264Params(fps, bitrate_, /*gopSeconds=*/2);
    streamer_->setCodec(useH264_ ? StreamCodec::H264 : StreamCodec::RawFrames);
}

void HostController::setReceiveDir(const QString &dir)
{
    if (dir.isEmpty())
        return;
    receiveDir_ = dir;
    if (fileReceiver_)
        fileReceiver_->setDownloadDir(dir);
}

void HostController::ensureFileReceiver()
{
    if (fileReceiver_) {
        fileReceiver_->setDownloadDir(receiveDir_);
        return;
    }

    fileReceiver_ = new FileReceiver(receiveDir_, this);

    // 事件与数字往下送，文案 / 进度条由 UI 侧做（本类不碰界面）。
    connect(fileReceiver_, &FileReceiver::started, this,
            [this](FileReceiver::Source, const QString &name, quint64 total) {
                emit fileStarted(name, total);
            });
    connect(fileReceiver_, &FileReceiver::progress, this,
            [this](FileReceiver::Source, quint64 received, quint64 total) {
                emit fileProgress(received, total);
            });
    connect(fileReceiver_, &FileReceiver::finished, this,
            [this](bool ok, const QString &path, const QString &message) {
                emit fileReceived(ok, path, message);
            });
}

QRect HostController::targetMonitorRect() const
{
    // 优先用「实际被抓的那块屏」：多显示器时归一化坐标必须还原到同一块屏，
    // 否则远程光标会跑到另一台显示器上。
    if (streamer_) {
        const QRect captured = streamer_->capturedRect();
        if (captured.isValid() && !captured.isEmpty())
            return captured;
    }

    // 退化情形（还没抓到首帧）：用虚拟桌面。
    // 注意不能改用 QScreen::geometry()——那是 Qt 逻辑像素，而 SendInput 与
    // DXGI 都用物理像素，本进程是 Per-Monitor DPI Aware，两者相差一个缩放比。
    return virtualDesktopRect();
}

quint64 HostController::framesSent() const
{
    return streamer_ ? streamer_->framesSent() : 0;
}

quint64 HostController::bytesSent() const
{
    return streamer_ ? streamer_->bytesSent() : 0;
}

QString HostController::sourceInfo() const
{
    return streamer_ ? streamer_->sourceInfo() : QString();
}

QString HostController::codecInfo() const
{
    return streamer_ ? streamer_->codecInfo() : QString();
}

} // namespace lanrdp
