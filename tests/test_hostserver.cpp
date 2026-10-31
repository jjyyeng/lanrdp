#include <QtTest/QtTest>

#include <QDateTime>
#include <QElapsedTimer>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "net/Auth.h"
#include "net/HostServer.h"
#include "net/Session.h"

// 连接上限（默认 4，满员后在鉴权通过前拒绝）
// 踢出接口（dropClient / dropAllClients）
// 心跳 + 死链清理
class TestHostServer : public QObject
{
    Q_OBJECT

private slots:
    void defaultMaxClientsIsFour();
    void rejectsConnectionBeyondMaxClients();
    void dropClientRemovesOnlyThatSession();
    void dropAllClientsClearsSessionsButKeepsListening();
    void heartbeatDefaultsMatchSpec();
    void heartbeatDropsSilentClientButKeepsResponsive();

    // 被拒会话的**生存期**（不只是池大小）
    void rejectedSessionIsReclaimed();

    // 登录失败退避（⑭）
    void backoffBlocksAfterRepeatedFailures();
    void throttledConnectionIsNotReportedAsRejected();
    void throttledConnectionStillReceivesDenyFrame();
    void backoffDoesNotCountCapacityRejections();
    void successfulAuthClearsBackoff();

    // listen() 契约：同实例改端口重监听得真的换过去；失败要给出可行动的原因
    void listenOnNewPortRebinds();
    void listenFailureReportsPortInUse();

    // 未认证连接的两道门：瞬时上限 + 握手超时
    void unauthenticatedConnectionsAreCapped();
    void handshakeTimeoutDropsSilentConnection();
};

namespace {

// 连一个客户端并启动鉴权握手（调用方负责接管返回的三个对象的生命周期）。
struct TestClient
{
    QTcpSocket *sock = nullptr;
    lanrdp::Session *session = nullptr;
    lanrdp::AuthClient *auth = nullptr;
};

TestClient connectClient(quint16 port)
{
    TestClient c;
    c.sock = new QTcpSocket();
    c.sock->connectToHost(QHostAddress::LocalHost, port);
    if (!c.sock->waitForConnected(3000))
        return c;

    c.session = new lanrdp::Session(c.sock);
    c.auth    = new lanrdp::AuthClient(c.session, QStringLiteral("pw"));
    c.auth->start();
    return c;
}

// host.sessions() 里是**服务端**的 Session（与测试里客户端侧的 Session 是两个对象），
// 用「服务端会话的对端端口 == 客户端套接字的本地端口」来配对。
lanrdp::Session *serverSessionFor(lanrdp::HostServer &host, const QTcpSocket *clientSock)
{
    for (lanrdp::Session *s : host.sessions()) {
        if (s->socket() && clientSock && s->socket()->peerPort() == clientSock->localPort())
            return s;
    }
    return nullptr;
}

} // namespace

void TestHostServer::defaultMaxClientsIsFour()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    QCOMPARE(host.maxClients(), 4);
}

void TestHostServer::rejectsConnectionBeyondMaxClients()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    host.setMaxClients(2);
    QVERIFY(host.listen(0));
    QVERIFY(host.port() != 0);

    int serverAuthed   = 0;
    int serverRejected = 0;
    QObject::connect(&host, &lanrdp::HostServer::clientAuthenticated,
                     [&serverAuthed]() { ++serverAuthed; });
    QObject::connect(&host, &lanrdp::HostServer::clientRejected,
                     [&serverRejected]() { ++serverRejected; });

    // 并发连接 3 个客户端，口令都正确：前 2 个应通过，第 3 个应被满员拒绝。
    int clientOk     = 0;
    int clientFailed = 0;

    QVector<QTcpSocket *>     sockets;
    QVector<lanrdp::Session *>   sessions;
    QVector<lanrdp::AuthClient *> auths;

    for (int i = 0; i < 3; ++i) {
        auto *sock = new QTcpSocket();
        sock->connectToHost(QHostAddress::LocalHost, host.port());
        QVERIFY(sock->waitForConnected(3000));

        auto *session = new lanrdp::Session(sock);
        auto *auth    = new lanrdp::AuthClient(session, QStringLiteral("pw"));
        QObject::connect(auth, &lanrdp::AuthClient::succeeded, [&clientOk]() { ++clientOk; });
        QObject::connect(auth, &lanrdp::AuthClient::failed, [&clientFailed]() { ++clientFailed; });
        auth->start();

        sockets.append(sock);
        sessions.append(session);
        auths.append(auth);
    }

    QTRY_COMPARE(serverAuthed, 2);
    QTRY_COMPARE(serverRejected, 1);
    QTRY_COMPARE(clientOk, 2);
    QTRY_COMPARE(clientFailed, 1);

    // 被拒者从未进入会话池
    QCOMPARE(host.sessions().size(), 2);

    qDeleteAll(auths);
    qDeleteAll(sessions);  // Session 析构连带释放其 socket
    sockets.clear();
}

// 踢掉其中一个控制端，另一个不受影响
void TestHostServer::dropClientRemovesOnlyThatSession()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    QVERIFY(host.listen(0));
    QVERIFY(host.port() != 0);

    TestClient a = connectClient(host.port());
    TestClient b = connectClient(host.port());
    QVERIFY(a.sock && b.sock);
    QTRY_COMPARE(host.sessions().size(), 2);

    lanrdp::Session *sa = serverSessionFor(host, a.sock);
    lanrdp::Session *sb = serverSessionFor(host, b.sock);
    QVERIFY(sa && sb);
    QVERIFY(sa != sb);

    // 下面一律用 id 说话：sa 稍后会被 deleteLater 回收，指针不能再拿去做判断。
    const quint64 idA = sa->id();
    const quint64 idB = sb->id();
    QVERIFY2(idA != idB, "同一进程里的两个会话不能共用 id");
    auto poolHasId = [&host](quint64 id) {
        for (lanrdp::Session *s : host.sessions()) {
            if (s->id() == id)
                return true;
        }
        return false;
    };

    int serverDisconnects = 0;
    QObject::connect(&host, &lanrdp::HostServer::clientDisconnected,
                     [&serverDisconnects]() { ++serverDisconnects; });

    bool aClosed = false;
    QObject::connect(a.sock, &QTcpSocket::disconnected, [&aClosed]() { aClosed = true; });

    host.dropClient(idA);  // a 被踢

    QTRY_COMPARE(host.sessions().size(), 1);
    QTRY_VERIFY(aClosed);
    QVERIFY(!poolHasId(idA));
    QVERIFY(poolHasId(idB));  // b 不受影响
    QCOMPARE(serverDisconnects, 1);

    // 重复踢同一个（已不在池中）应当是安全 no-op；0 是无效 id，同样安全
    host.dropClient(idA);
    host.dropClient(0);
    QCOMPARE(host.sessions().size(), 1);
    QCOMPARE(serverDisconnects, 1);

    // 陈旧 id 不能踢到**新**会话 —— 这正是按 id 而不是按裸指针的理由
    TestClient c = connectClient(host.port());
    QVERIFY(c.sock);
    QTRY_COMPARE(host.sessions().size(), 2);
    host.dropClient(idA);   // a 早没了，这个号已经指向不了任何在池会话
    QCOMPARE(host.sessions().size(), 2);
    QVERIFY(poolHasId(idB));

    delete c.auth;
    delete c.session;

    // sa 已由 HostServer 走 deleteLater 回收；b 的客户端侧对象由本用例负责
    delete b.auth;
    delete b.session;
}

// 一次踢掉全部控制端，但监听保持（可以再接新控制端）
void TestHostServer::dropAllClientsClearsSessionsButKeepsListening()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    host.setMaxClients(3);
    QVERIFY(host.listen(0));

    TestClient c1 = connectClient(host.port());
    TestClient c2 = connectClient(host.port());
    TestClient c3 = connectClient(host.port());
    QVERIFY(c1.sock && c2.sock && c3.sock);
    QTRY_COMPARE(host.sessions().size(), 3);

    int serverDisconnects = 0;
    QObject::connect(&host, &lanrdp::HostServer::clientDisconnected,
                     [&serverDisconnects]() { ++serverDisconnects; });

    host.dropAllClients();

    QTRY_COMPARE(host.sessions().size(), 0);
    QTRY_COMPARE(serverDisconnects, 3);
    QVERIFY(host.isListening());  // 仍在听，只是把现有连接都断了

    // 三个 session 都已交给 HostServer 走 deleteLater，这里不再手动 delete。
}

// 心跳/死链阈值的默认值必须与规格一致（3 s 发、10 s 判死）。
void TestHostServer::heartbeatDefaultsMatchSpec()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    QCOMPARE(host.heartbeatIntervalMs(), 3000);
    QCOMPARE(host.deadTimeoutMs(), 10000);
}

// 被控端用**一个统一**的心跳定时器遍历会话 ——
// ① 只连上、之后不回应的「哑客户端」在死链阈值内被清出会话池；
// ② 收到心跳就回一帧的正常客户端不会被误杀。
// 这才是这一版真正要治的病：Wi-Fi 抖动 / 对端进程被强杀时 TCP 不产生 disconnected，
// 界面会一直停在「接收中」的假象里。
void TestHostServer::heartbeatDropsSilentClientButKeepsResponsive()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    host.setHeartbeatIntervalMs(50);    // 测试里把 3 s / 10 s 压成 50 ms / 150 ms
    host.setDeadTimeoutMs(150);
    QVERIFY(host.listen(0));

    // 哑客户端：握手完就不再发任何东西（但 socket 还开着）
    TestClient dumb = connectClient(host.port());
    QVERIFY(dumb.sock);
    QTRY_COMPARE(host.sessions().size(), 1);

    // 正常客户端：收到 Heartbeat 立刻回一帧
    TestClient live = connectClient(host.port());
    QVERIFY(live.sock);
    QObject::connect(live.session, &lanrdp::Session::frameReceived,
                     [&live](const lanrdp::Frame &f) {
                         if (f.type == static_cast<std::uint8_t>(lanrdp::FrameType::Heartbeat))
                             live.session->sendFrame(
                                 static_cast<std::uint8_t>(lanrdp::FrameType::Heartbeat));
                     });
    QTRY_COMPARE(host.sessions().size(), 2);

    int serverDisconnects = 0;
    QObject::connect(&host, &lanrdp::HostServer::clientDisconnected,
                     [&serverDisconnects]() { ++serverDisconnects; });

    // ① 哑客户端被清出（阈值 150 ms，给足余量）
    QTRY_VERIFY_WITH_TIMEOUT(host.sessions().size() == 1, 3000);
    QCOMPARE(serverDisconnects, 1);
    QVERIFY2(host.sessions().contains(serverSessionFor(host, live.sock)),
             "会回应心跳的客户端不该被踢");
    QVERIFY2(!host.sessions().contains(serverSessionFor(host, dumb.sock)),
             "不回应心跳的客户端应当已被清出会话池");

    // ② 再观察一段：正常客户端不该被后续心跳误杀
    QTest::qWait(500);
    QCOMPARE(host.sessions().size(), 1);
    QCOMPARE(serverDisconnects, 1);

    delete live.auth;
    delete live.session;
    delete dumb.auth;
    delete dumb.session;   // 服务端那份已由 HostServer 走 deleteLater
}

// 被拒会话**从未进入会话池**，
// 但同样必须被回收 —— 否则每次口令打错 / 端口扫描 / 健康检查都会永久泄漏
// 一组 Session + socket（TCP 连接已经关了，server 端对象却还在）。
//
// 原来 `removeSession` 的第一句是 `if (sessions_.removeAll(s) == 0) return;`，
// 那把紧随其后的 `deleteLater()` 一起跳过了；而 `Session::disconnected` 正是
// 通过这条路径回收的 —— 注释写着「已由 HostServer 走 deleteLater」，实现没走。
//
// 判据用 `QPointer`（能观察「对象是否已死」），而不是只看池大小
// —— 池大小断言在这条路径上恒为 0，什么也守不住。
// 判据分两步，缺一不可：
// ① `fired` —— 证明「被拒」这条路确实走到了（排除「压根没发生」的假绿）；
// ② `alive` 归空 —— 证明服务端那个 Session 对象**真的死了**。
// 不直接断言「中间态非空」：回收发生在毫秒级，轮询式断言观测不到那个瞬时状态。
void TestHostServer::rejectedSessionIsReclaimed()
{
    lanrdp::HostServer host(QStringLiteral("right-pw"));
    QVERIFY(host.listen(0));

    bool                      fired = false;
    QPointer<lanrdp::Session> alive;   // 只用来观察「对象是否已死」
    QObject::connect(&host, &lanrdp::HostServer::clientRejected,
                     [&fired, &alive](lanrdp::Session *s) {
                         fired = true;
                         alive = s;
                     });

    // 口令错误的客户端：服务端回 DENY 后立即断开。
    auto *sock = new QTcpSocket();
    sock->connectToHost(QHostAddress::LocalHost, host.port());
    QVERIFY(sock->waitForConnected(3000));

    auto *session = new lanrdp::Session(sock);
    auto *auth    = new lanrdp::AuthClient(session, QStringLiteral("wrong-pw"));
    auth->start();

    QTRY_VERIFY2(fired, "口令错误应当触发 clientRejected");
    QCOMPARE(host.sessions().size(), 0);   // 被拒者从未入池
    QTRY_VERIFY2(alive.isNull(),
                 "被拒会话必须被回收 —— 否则每次失败连接都永久泄漏一组 Session+socket");

    delete auth;
    delete session;   // 客户端侧那份（服务端那份已由 HostServer 回收）
}

// ---------------------------------------------------------------------------
// 登录失败退避（⑭）
// ---------------------------------------------------------------------------

namespace {

// 用**错误口令**连一次并等它被拒（不退避时走的是正常拒绝路径）。
// 返回是否确实收到了拒绝。
//
// 这里手写轮询而不用 QTRY_VERIFY_WITH_TIMEOUT：后者展开后含 `return;`
// （它是给 void 测试槽用的），放进非 void 函数会直接编译失败。
bool attemptWithWrongPassword(lanrdp::HostServer &host, const QString &wrongPassword)
{
    auto *sock = new QTcpSocket();
    sock->connectToHost(QHostAddress::LocalHost, host.port());
    if (!sock->waitForConnected(3000))
        return false;

    auto *session = new lanrdp::Session(sock);
    auto *auth    = new lanrdp::AuthClient(session, wrongPassword);

    bool failed = false;
    QObject::connect(auth, &lanrdp::AuthClient::failed, [&failed]() { failed = true; });
    auth->start();

    QElapsedTimer timer;
    timer.start();
    while (!failed && timer.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    delete auth;      // 客户端侧对象（服务端那份由 HostServer 回收）
    delete session;
    return failed;
}

} // namespace

// 连续失败若干次后，**新连接**在建立时就被挡下 —— 不发 clientRejected，
// 而是发 connectionThrottled。判据用信号而不是「等多久」，因为后者不稳。
void TestHostServer::backoffBlocksAfterRepeatedFailures()
{
    lanrdp::HostServer host(QStringLiteral("right-pw"));
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs   = 30000;   // 第一次失败就锁 30 s —— 测试期间绝不会自然到期
    cfg.maxDelayMs    = 30000;
    cfg.resetWindowMs = 60000;
    host.setBackoffConfig(cfg);
    QVERIFY(host.listen(0));

    int  throttledCount = 0;
    QString throttledIp;
    QObject::connect(&host, &lanrdp::HostServer::connectionThrottled,
                     [&throttledCount, &throttledIp](const QString &ip, qint64) {
                         ++throttledCount;
                         throttledIp = ip;
                     });

    QVERIFY(attemptWithWrongPassword(host, QStringLiteral("bad")));
    QCOMPARE(throttledCount, 0);   // 第一次失败只是记账，本次连接仍走正常拒绝

    // 再来一条 —— 这次在建立时就被挡。
    auto *sock = new QTcpSocket();
    sock->connectToHost(QHostAddress::LocalHost, host.port());
    QVERIFY(sock->waitForConnected(3000));

    QTRY_VERIFY_WITH_TIMEOUT(throttledCount >= 1, 3000);
    QVERIFY2(!throttledIp.isEmpty(), "被拦的来源 IP 应被报出");
    QCOMPARE(host.sessions().size(), 0);

    // 被拦的连接不该留在 pending 队列里被反复受理。
    sock->waitForDisconnected(3000);
    sock->deleteLater();
}

// 被退避挡下的连接**不是**「认证被拒」—— 两条路径语义不同，混在一起
// 会让上层把端口扫描统计成「有人口令输错了」，也会污染 clientRejected 的计数。
void TestHostServer::throttledConnectionIsNotReportedAsRejected()
{
    lanrdp::HostServer host(QStringLiteral("right-pw"));
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs   = 30000;
    cfg.maxDelayMs    = 30000;
    cfg.resetWindowMs = 60000;
    host.setBackoffConfig(cfg);
    QVERIFY(host.listen(0));

    int rejected   = 0;
    int throttled  = 0;
    QObject::connect(&host, &lanrdp::HostServer::clientRejected, [&rejected]() { ++rejected; });
    QObject::connect(&host, &lanrdp::HostServer::connectionThrottled,
                     [&throttled](const QString &, qint64) { ++throttled; });

    QVERIFY(attemptWithWrongPassword(host, QStringLiteral("bad")));
    QCOMPARE(rejected, 1);      // 这一次是真正的认证失败
    QCOMPARE(throttled, 0);

    // 再来一条：被退避挡下 —— 只加 throttled，**不加** rejected。
    auto *sock = new QTcpSocket();
    sock->connectToHost(QHostAddress::LocalHost, host.port());
    QVERIFY(sock->waitForConnected(3000));
    QTRY_COMPARE(throttled, 1);
    QCOMPARE(rejected, 1);      // 没涨

    sock->deleteLater();
}

// 被退避挡下的连接必须让对端**真的收到** HANDSHAKE_DENY 帧。
//
// 缺陷形态：被控端写 DENY 后立刻关闭连接，而客户端连上即发 HANDSHAKE_REQ
// （真实客户端 AuthClient::start 就发这一帧）。这份请求在关闭之后到达，
// TCP 会回 RST；RST 到达时对端接收缓冲里那帧 DENY 会被一并丢弃，于是客户端
// 只看到「远端关闭了连接」，无从得知是限流（见 net/HostServer.cpp 节流分支）。
void TestHostServer::throttledConnectionStillReceivesDenyFrame()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs   = 30000;   // 一次失败即锁死整轮测试
    cfg.maxDelayMs    = 30000;
    cfg.resetWindowMs = 60000;
    host.setBackoffConfig(cfg);
    QVERIFY(host.listen(0));

    QVERIFY(attemptWithWrongPassword(host, QStringLiteral("bad")));  // 先撞出退避

    auto *sock    = new QTcpSocket();
    auto *session = new lanrdp::Session(sock);

    bool denied = false;
    QObject::connect(session, &lanrdp::Session::frameReceived,
                     [&denied](const lanrdp::Frame &f) {
                         if (f.type == static_cast<std::uint8_t>(lanrdp::FrameType::HandshakeDeny))
                             denied = true;
                     });

    sock->connectToHost(QHostAddress::LocalHost, host.port());
    QVERIFY(sock->waitForConnected(3000));

    // 复刻真实客户端：连上立刻声明（AuthClient::start 发的就是这一帧）。
    lanrdp::Frame req;
    req.type = static_cast<std::uint8_t>(lanrdp::FrameType::HandshakeReq);
    sock->write(req.serialize());
    sock->flush();

    QElapsedTimer timer;
    timer.start();
    while (!denied && timer.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    QVERIFY2(denied, "节流连接没收到 HANDSHAKE_DENY —— DENY 帧被 RST 冲掉了");

    // 回完 DENY 还得真断开，否则被拦的连接会一直挂着占资源。
    QTRY_VERIFY_WITH_TIMEOUT(sock->state() == QAbstractSocket::UnconnectedState, 3000);

    delete session;   // socket 归 Session 所有，随之回收
}

// 满员拒绝**不记账** —— 否则攻击者用错误口令刷满 4 个名额就能把机主锁在门外。
void TestHostServer::backoffDoesNotCountCapacityRejections()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    host.setMaxClients(1);
    QVERIFY(host.listen(0));

    // 第 1 个用**正确**口令占住名额。
    TestClient first = connectClient(host.port());
    QVERIFY(first.sock);
    QTRY_COMPARE(host.sessions().size(), 1);

    int rejected = 0;
    QObject::connect(&host, &lanrdp::HostServer::clientRejected, [&rejected]() { ++rejected; });

    // 第 2、3 个用**正确**口令但会被满员拦下 —— 这条路径不能进退避账本。
    // 用正确口令是有意的：这样 rejected 只可能来自容量判定。
    for (int i = 0; i < 2; ++i) {
        auto *sock = new QTcpSocket();
        sock->connectToHost(QHostAddress::LocalHost, host.port());
        QVERIFY(sock->waitForConnected(3000));
        auto *session = new lanrdp::Session(sock);
        auto *auth    = new lanrdp::AuthClient(session, QStringLiteral("pw"));
        bool   done   = false;
        QObject::connect(auth, &lanrdp::AuthClient::failed, [&done]() { done = true; });
        auth->start();
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        delete auth;
        delete session;
    }

    QCOMPARE(rejected, 2);

    // 关键断言：退避账本里**没有**任何来源被记账（满员拒绝不该产生惩罚）。
    QVERIFY2(host.backoff().isAllowed(QStringLiteral("127.0.0.1"), 1000),
             "满员拒绝被误记进了退避账本 —— 攻击者可用错误口令把机主锁在门外");
    QCOMPARE(host.backoff().failureCount(QStringLiteral("127.0.0.1"), 1000), 0);

    delete first.auth;
    delete first.session;
}

// 成功一次即清零。baseDelayMs = 0 —— 计数照记、但「惩罚」为 0，
// 于是失败若干次后立刻就能成功，不必真等，也不会撞上「恰好还在锁里」的竞态。
void TestHostServer::successfulAuthClearsBackoff()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs   = 0;       // 惩罚 0 ms：等于「只记账、不拦人」
    cfg.maxDelayMs    = 0;
    cfg.resetWindowMs = 600000;  // 窗口拉长，排除「自然清零」的干扰
    host.setBackoffConfig(cfg);
    QVERIFY(host.listen(0));

    const QString ip = QStringLiteral("127.0.0.1");

    QVERIFY(attemptWithWrongPassword(host, QStringLiteral("bad")));
    QVERIFY(attemptWithWrongPassword(host, QStringLiteral("bad")));
    QCOMPARE(host.backoff().failureCount(ip, QDateTime::currentMSecsSinceEpoch()), 2);

    // 正确口令连一次 → 进池，且**必须把该来源的失败计数清零**。
    TestClient ok = connectClient(host.port());
    QVERIFY(ok.sock);
    QTRY_COMPARE(host.sessions().size(), 1);

    QCOMPARE(host.backoff().failureCount(ip, QDateTime::currentMSecsSinceEpoch()), 0);

    delete ok.auth;
    delete ok.session;
}

// 未认证连接不能无限堆 —— 它们既不在 sessions_（心跳判死看不见），也不占 maxClients_ 名额。
// 判据：超过 maxPendingAuth() 的那条「连上就被掐」，而且这**不算**「认证被拒」（语义不同，
// 混在一起会让上层把扫描流量统计成「有人输错口令」）。
void TestHostServer::unauthenticatedConnectionsAreCapped()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    host.setMaxClients(2);
    host.setMaxPendingAuth(3);
    QCOMPARE(host.maxPendingAuth(), 3);
    QVERIFY(host.listen(0));

    // 3 条只连不握手：正好占满未认证名额
    QVector<QTcpSocket *> silent;
    for (int i = 0; i < 3; ++i) {
        auto *s = new QTcpSocket();
        s->connectToHost(QHostAddress::LocalHost, host.port());
        QVERIFY(s->waitForConnected(3000));
        silent.append(s);
    }
    QTRY_COMPARE(host.pendingAuthCount(), 3);
    for (QTcpSocket *s : silent)
        QCOMPARE(s->state(), QAbstractSocket::ConnectedState);

    // 第 4 条超上限：必须被立刻断开
    int rejected = 0;
    QObject::connect(&host, &lanrdp::HostServer::clientRejected,
                     [&rejected]() { ++rejected; });

    QTcpSocket extra;
    extra.connectToHost(QHostAddress::LocalHost, host.port());
    extra.waitForConnected(2000);   // 被立刻掐掉时这里可能就是 false，两种都接受

    QTRY_COMPARE(extra.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(rejected, 0);                 // 不是「口令错」，也不是「满员被拒」
    QCOMPARE(host.sessions().size(), 0);   // 一条都没进池
    QCOMPARE(host.pendingAuthCount(), 3);  // 僵尸仍在（还没到超时），但名额没被撑爆

    qDeleteAll(silent);
}

// 「连上却不握手」= 僵尸：必须被超时放掉；同一时刻正常握手的连接不能被误伤。
void TestHostServer::handshakeTimeoutDropsSilentConnection()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    host.setHandshakeTimeoutMs(800);
    QCOMPARE(host.handshakeTimeoutMs(), 800);
    QVERIFY(host.listen(0));

    QTcpSocket silent;
    silent.connectToHost(QHostAddress::LocalHost, host.port());
    QVERIFY(silent.waitForConnected(3000));

    // 同时来一个正常客户端：它要能在超时窗口内握手成功
    TestClient ok = connectClient(host.port());
    QVERIFY(ok.sock);
    QTRY_COMPARE(host.sessions().size(), 1);
    QCOMPARE(host.pendingAuthCount(), 1);   // 未出结论的只剩那条静默的

    // 静默连接必须被断开（abort，不等对端 FIN）
    QTRY_COMPARE(silent.state(), QAbstractSocket::UnconnectedState);

    // 已认证的连接不受影响，pending 记账也归零
    QCOMPARE(host.sessions().size(), 1);
    QCOMPARE(ok.sock->state(), QAbstractSocket::ConnectedState);
    QTRY_COMPARE(host.pendingAuthCount(), 0);

    delete ok.auth;
    delete ok.session;
}

// 同一实例、不同端口再 listen：必须真的换到新端口。
// 缺陷形态是「已监听就直接 return true」—— 新端口被静默丢掉，现象是
// 「旧端口还能连、新端口连不上」，很容易被误判成「防火墙拦了」。
void TestHostServer::listenOnNewPortRebinds()
{
    lanrdp::HostServer host(QStringLiteral("pw"));
    QVERIFY(host.listen(0));
    const quint16 first = host.port();
    QVERIFY(first != 0);

    // 同参数重复 listen 是幂等的（不能因为「已在监听」就报错）
    QVERIFY(host.listen(first));
    QCOMPARE(host.port(), first);

    // 挑一个与当前不同的空闲端口
    quint16 second = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::Any, 0));
        second = probe.serverPort();
        probe.close();
    }
    QVERIFY(second != first);

    QVERIFY2(host.listen(second), "改端口重新监听应当成功");
    QCOMPARE(host.port(), second);
    QVERIFY(host.isListening());

    // 新端口真的能连上 —— 而不是只改了「账面上的端口」
    QTcpSocket sock;
    sock.connectToHost(QHostAddress::LocalHost, second);
    QVERIFY2(sock.waitForConnected(3000), qPrintable(sock.errorString()));
}

// 端口被占用时，失败原因必须「可行动」：点出是哪个端口、以及该怎么办 ——
// 而不是只印 QTcpServer 的 "The address is already in use"（用户不知道该关谁）。
void TestHostServer::listenFailureReportsPortInUse()
{
    QTcpServer occupier;
    QVERIFY(occupier.listen(QHostAddress::Any, 0));
    const quint16 busy = occupier.serverPort();
    QVERIFY(busy != 0);

    lanrdp::HostServer host(QStringLiteral("pw"));
    QSignalSpy          spy(&host, &lanrdp::HostServer::listenFailed);

    QVERIFY(!host.listen(busy));
    QVERIFY(!host.isListening());
    QCOMPARE(spy.count(), 1);

    const QString reason = spy.first().at(1).toString();
    QVERIFY2(reason.contains(QString::number(busy)), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("占用")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("换")) || reason.contains(QStringLiteral("关掉")),
             qPrintable(reason));
}

QTEST_GUILESS_MAIN(TestHostServer)
#include "test_hostserver.moc"
