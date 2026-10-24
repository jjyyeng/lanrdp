#include "net/HostServer.h"

#include "net/Auth.h"
#include "net/Session.h"

#include <QDateTime>
#include <QAbstractSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace {

// 退避分桶键 = 源 IP 的规范字符串。
//
// ⚠️ 直接 `peerAddress().toString()` 在双栈监听下会给出 IPv4-mapped 形式
// （实测 `::ffff:127.0.0.1` 而不是 `127.0.0.1`）—— 分桶本身仍自洽，但日志/界面
// 可读性差，且「同一台机器的 v4 与 v6 连接」会变成两个不同的桶。
// 这里统一归一化：能转成 IPv4（含 mapped 形式）就转，转不了才保留 v6 原文。
QString sourceIpKey(const QHostAddress &address)
{
    if (address.isNull())
        return {};
    // 注意不能用 isIPv4() 做判据：它对 `::ffff:a.b.c.d` 返回 false，
    // 而那正是双栈监听下最常出现的形式。
    bool             ok = false;
    const quint32    v4 = address.toIPv4Address(&ok);
    if (ok)
        return QHostAddress(v4).toString();
    return address.toString();
}

// 把「监听失败」翻译成一句用户能照着做的话。
//
// AddressInUseError 是最常见的一种：光印 errorString() 只会说
// "The address is already in use"，用户既不知道是谁占的，也不知道该干什么。
QString listenFailureReason(QTcpServer *server, quint16 port)
{
    if (server->serverError() == QAbstractSocket::AddressInUseError) {
        return QStringLiteral("端口 %1 已被占用：另一个程序（或本程序的另一个实例）正在使用它。"
                              "请换一个端口，或先关掉占用该端口的程序。")
            .arg(port);
    }
    return QStringLiteral("监听端口 %1 失败：%2").arg(port).arg(server->errorString());
}

// 被退避挡下的连接：回一帧 HANDSHAKE_DENY 后断开。
//
// 不能「写完帧立刻 close」：客户端连上就会发它的握手请求，而此刻这条连接还
// 一个字节都没读过。若直接关闭，那份请求会打在已关闭的套接字上 —— 内核回一个
// RST，而 RST 会把对端刚收到的 DENY 一并作废，客户端只报「远端关闭了连接」，
// 看不出是限流（这正是本函数存在的唯一理由）。
// 做法：等到对端数据（readyRead）先把它读干净，再回 DENY、再关；对端若一声
// 不发（端口扫描），由宽限定时器兜底，不把连接一直晾着。
constexpr int kThrottleRejectGraceMs = 200;

void rejectThrottledConnection(QTcpSocket *socket)
{
    // 回收交给 disconnected 信号 —— 不能自己 deleteLater：QTcpSocket 的关闭是
    // 异步的（要等写缓冲排空才真正关闭），提前析构会把没发出去的 DENY 一起丢掉。
    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);

    auto *grace = new QTimer(socket);   // parent = socket：随连接一并回收
    grace->setSingleShot(true);
    grace->setInterval(kThrottleRejectGraceMs);

    // 收尾只做一次 —— 拿「定时器是否还在跑」当标志（readyRead 与超时都会进来）。
    auto finish = [socket, grace]() {
        if (!grace->isActive())
            return;
        grace->stop();
        socket->readAll();                 // 读空对端残留字节，杜绝 close 触发 RST
        lanrdp::Frame deny;
        deny.type = static_cast<std::uint8_t>(lanrdp::FrameType::HandshakeDeny);
        socket->write(deny.serialize());
        socket->disconnectFromHost();      // 等出站帧写完再真正关闭
    };

    QObject::connect(grace, &QTimer::timeout, socket, finish);
    QObject::connect(socket, &QTcpSocket::readyRead, socket, finish);
    grace->start();
}

} // namespace

namespace lanrdp {

HostServer::HostServer(QString password, QObject *parent)
    : QObject(parent)
    , password_(std::move(password))
{
}

bool HostServer::listen(quint16 port, const QHostAddress &address)
{
    if (!server_) {
        server_ = new QTcpServer(this);
        connect(server_, &QTcpServer::newConnection, this, &HostServer::onNewConnection);
    }

    if (server_->isListening()) {
        // 已经监听：只有请求的端口/地址与当前**完全一致**时才当作幂等成功（重复调用
        // listen(同参数) 是有意的容错）。不一致就必须先停再起 —— 否则调用方改了端口
        // 却仍在旧端口上监听，表现为「旧端口还能连、新端口连不上」。
        const bool samePort = (port == 0 || port == server_->serverPort());
        // 把 QHostAddress::Any 单独认一下：Any 与 serverAddress() 可能给出
        // 语义相同但 protocol 字段不同的值（Any 是「任意协议」，AnyIPv4 是 0.0.0.0），
        // 直接比会误判成「换了地址」而多一次无谓的重绑定。
        const bool sameAddr = address.isNull() || address == QHostAddress::Any
                              || address == server_->serverAddress();
        if (samePort && sameAddr)
            return true;

        if (heartbeatTimer_)
            heartbeatTimer_->stop();   // 换端口期间先别发心跳
        server_->close();
    }

    if (!server_->listen(address, port)) {
        emit listenFailed(port, listenFailureReason(server_, port));
        // 换端口失败时，上面已经关掉了旧监听 —— 但已建立的会话不受影响（关的是
        // 监听套接字，不是那些连接），它们仍在收画面。可心跳刚被停掉了，死链就
        // 再没人判，于是一条早已断掉的连接会一直占着名额。还有会话在就把它续上。
        if (!sessions_.isEmpty() && heartbeatTimer_)
            heartbeatTimer_->start();
        return false;
    }

    // 一个统一的 3 s 心跳定时器（不是每会话一个 —— 会话数上限 4，
    // 统一遍历更省事，也不会随连接数增长堆定时器）。
    if (!heartbeatTimer_) {
        heartbeatTimer_ = new QTimer(this);
        connect(heartbeatTimer_, &QTimer::timeout, this, &HostServer::onHeartbeatTick);
    }
    heartbeatTimer_->setInterval(heartbeatIntervalMs_);
    heartbeatTimer_->start();

    emit listening(server_->serverPort());
    return true;
}

void HostServer::stop()
{
    if (heartbeatTimer_)
        heartbeatTimer_->stop();   // 停监听就别再发心跳了
    if (server_ && server_->isListening())
        server_->close();
}

// 给每个会话发一帧心跳，顺手把「久无音信」的会话判死踢掉。
// 判死依据是「收到任何字节」而不只是「收到心跳帧」——对端在推画面却不理心跳
// 这种半死状态，也会因为画面帧持续到达而被视为活着（这是有意的）。
void HostServer::onHeartbeatTick()
{
    const QVector<Session *> snapshot = sessions_;  // dropSession 会改 sessions_
    for (Session *s : snapshot) {
        if (!sessions_.contains(s))  // 同一轮里已被前面的清理带走了
            continue;

        s->sendFrame(static_cast<std::uint8_t>(FrameType::Heartbeat));

        if (s->msSinceLastRx() > deadTimeoutMs_)
            abortSession(s);
    }
}

bool HostServer::isListening() const
{
    return server_ && server_->isListening();
}

// 踢出单个控制端（对外接口）。按 **id** 而不是指针查池：调用方手里的可能是
// 一份早先的快照，那条会话可能已经释放、地址又被新连接复用（ABA）——
// 拿指针来查会踢错人，拿 id 来查最多是 no-op。
void HostServer::dropClient(quint64 sessionId)
{
    for (Session *s : sessions_) {
        if (s->id() == sessionId) {
            dropSession(s);
            return;
        }
    }
    // 不在池中：安全 no-op
}

// 优先走优雅断开（disconnectFromHost），让对端收到 FIN；
// 已经处于断开态的会话直接清理，避免 disconnectFromHost 不触发 disconnected 而留在池中。
void HostServer::dropSession(Session *session)
{
    if (!session || !sessions_.contains(session))
        return;

    QTcpSocket *sock = session->socket();
    if (!sock || sock->state() == QAbstractSocket::UnconnectedState) {
        removeSession(session);
        return;
    }
    sock->disconnectFromHost();
}

// 判死专用：立刻关闭，不等对端回 FIN。
//
// 与 dropSession 的区别只在「等不等排空」：判死的对端已经不回任何字节了，
// disconnectFromHost 会把会话挂在 ClosingState 直到 TCP 重传超时（Windows 默认
// 十几秒），这段时间它还占着 maxClients_ 名额，而且下一拍心跳会再判死一次。
// 控制端侧的同场景（ClientConnection::onHeartbeatTick）就是这么做的。
void HostServer::abortSession(Session *session)
{
    if (!session || !sessions_.contains(session))
        return;

    QTcpSocket *sock = session->socket();
    if (!sock || sock->state() == QAbstractSocket::UnconnectedState) {
        removeSession(session);
        return;
    }

    // abort 会立刻关闭并（在 connected 态下）同步发出 disconnected，
    // 由 Session::onDisconnected -> 本类的 removeSession 完成回收。
    // 后面再兜一次：万一当时已不在可发信号的态，abort 什么都不发，
    // 死会话就会永远留在池里。removeSession 幂等，多来一次是 no-op。
    sock->abort();
    if (sessions_.contains(session))
        removeSession(session);
}

void HostServer::dropAllClients()
{
    const QVector<Session *> snapshot = sessions_;  // dropSession 会改 sessions_
    for (Session *s : snapshot)
        dropSession(s);
}

// 会话离开池的**唯一出口**，且**无条件回收**。
// · 曾在池中 -> 发 clientDisconnected（保持「重复调用只发一次」的幂等语义）；
// · 不在池中 -> 不发信号，但**仍然 deleteLater**。
// 被判拒的会话从未入池，若在这里早返回，它就会永久泄漏（socket 已断，Session 还在）。
// 重复 deleteLater 是 Qt 明确支持的（第二次为 no-op），所以多路径命中不会双删。
void HostServer::removeSession(Session *session)
{
    if (!session)
        return;

    if (sessions_.removeAll(session) > 0)
        emit clientDisconnected(session);
    session->deleteLater();
}

quint16 HostServer::port() const
{
    return server_ ? server_->serverPort() : 0;
}

void HostServer::onNewConnection()
{
    while (server_->hasPendingConnections()) {
        QTcpSocket *socket = server_->nextPendingConnection();

        // 登录失败退避：**在新连接建立时就拦**。
        //
        // 为什么必须在这里、而不是在握手失败时把当前连接挂起来等：
        // 攻击者「断线重连」是零成本的，挂在旧连接上的惩罚等于不存在。
        //
        // 也刻意**不建 Session、不发 clientRejected** —— 这不是「认证被拒」，
        // 而是「根本不受理」；两者语义不同，混在一起会让上层把扫描流量
        // 统计成「有人口令输错了」。
        const QString sourceIp = sourceIpKey(socket->peerAddress());
        const qint64  nowMs    = QDateTime::currentMSecsSinceEpoch();
        if (!backoff_.isAllowed(sourceIp, nowMs)) {
            const qint64 waitMs = backoff_.remainingDelayMs(sourceIp, nowMs);
            emit connectionThrottled(sourceIp, waitMs);

            // 这条路径没有 Session，直接对裸 socket 收尾（为什么不能立刻 close
            // 见 rejectThrottledConnection 的函数头注释）。
            rejectThrottledConnection(socket);
            continue;
        }

        Session *session = new Session(socket, this);

        // 每个 Session 配一个握手器；握手器随 Session 一起销毁。
        AuthServer *auth = new AuthServer(password_, session);
        auth->attach(session);

        // 满员拒绝：鉴权通过（口令正确）但会话数已满时，在回 OK 之前拦截。
        // ⚠️ 这条路径**不记账到退避** —— 否则攻击者用错误口令刷满名额
        // 就能把机主自己锁在门外（把限流变成 DoS 工具）。
        auth->setAcceptGuard([this] { return sessions_.size() < maxClients_; });

        connect(auth, &AuthServer::authenticated, this, [this](Session *s) {
            // 认证成功即清零该来源的失败计数（同 IP 的下一次尝试不再受罚）。
            backoff_.noteSuccess(s->socket() ? sourceIpKey(s->socket()->peerAddress())
                                             : QString());
            // 去重：即便 AuthServer 的闩被绕过，同一会话也绝不入池两次 ——
            // 入池两次会让它独占名额，且上层每接一次收帧 lambda 就多注入一次输入。
            if (sessions_.contains(s))
                return;
            sessions_.append(s);
            emit clientAuthenticated(s);
        });

        connect(auth, &AuthServer::rejected, this, [this](Session *s) {
            // 只有「口令比对失败」才记账。满员被拒（acceptGuard_ 返回 false）
            // 同样会走 rejected 信号 —— 靠「池子还没满」反推它是不是容量拒绝。
            const bool capacityReject = sessions_.size() >= maxClients_;
            if (!capacityReject && s->socket())
                backoff_.noteFailure(sourceIpKey(s->socket()->peerAddress()),
                                     QDateTime::currentMSecsSinceEpoch());
            emit clientRejected(s);
            // 口令错误：回 DENY 后立即断开，不占用监听资源。
            if (s->socket())
                s->socket()->disconnectFromHost();
        });

        connect(session, &Session::disconnected, this, [this, session]() {
            removeSession(session);
        });
    }
}

} // namespace lanrdp
