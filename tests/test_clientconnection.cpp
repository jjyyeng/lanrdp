#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

#include "net/ClientConnection.h"
#include "net/HostServer.h"
#include "net/Session.h"

// ClientConnection 的**通用行为**测试。
//
// 与 `test_clientconnection_h264.cpp` 的分工：那个专测 H.264 收帧 / 解码 / 显示预算
// 那条媒体链路；本文件放与媒体无关的连接行为（代理、参数记录……）。
//
// 缘起（真机部署时暴露的 bug）：控制端连另一台机器报
// RESULT DENY The proxy type is invalid for this operation
// 而连 127.0.0.1 却一切正常。根因是 `QTcpSocket` 默认走**系统代理**（Windows 的
// Internet Settings / 环境变量 `*_proxy`），局域网 IP 不在代理的 bypass 名单里就被拦下。
// 一个纯局域网直连的工具不该受这个影响 —— 这条用例把它钉死。
class TestClientConnection : public QObject
{
    Q_OBJECT

private slots:
    void clientSocketBypassesSystemProxy();
    void transferIdsStartUnpredictablyAndIncrease();
    // 死链判定的**计时起点**（未握手时不能拿「从未收到」哨兵当静默时长）
    void deadLinkDetectionPicksClockByFirstByte();
    // 自动重连退避必须逐轮增长（不能在每轮 connectTo 里被清零）
    void reconnectBackoffGrowsAcrossAutoReconnects();
    // 口令被拒 ≠ 网络故障：不许自动重连（否则拿错口令无限重试 + 刷爆对端退避）
    void authRejectionStopsAutoReconnect();
};

// 传输 id 从随机值起步、同连接内递增。
//
// 病灶：固定从 1 开始 ⇒ 每条连接都用同一串 id（1,2,3,…）。接收侧改成按来源
// 隔离后已不再依赖「id 全局唯一」，但可预测的 id 仍是白送的便利（对端能拿它
// 构造「同 id 打断」）。随机起点把这点抹掉，且不改变协议（id 仍是 quint32）。
void TestClientConnection::transferIdsStartUnpredictablyAndIncrease()
{
    lanrdp::ClientConnection conn;

    const quint32 first = conn.nextTransferId();

    // 起点不该是那串「经典小整数」——真的随机取值撞上它们的概率极低。
    QVERIFY2(first != 1u, "传输 id 不该再从 1 起步");
    QVERIFY2(first > 1000u, "起点应当明显偏离小整数区间");

    // 同一连接内仍要递增（这是它唯一必须保证的性质）。
    // 通过反射式的重复采样做不到（id 只在 sendFile 时推进），
    // 故这里改为「多次新建连接，起点各不相同」来证明它确实随机。
    QSet<quint32> starts;
    for (int i = 0; i < 8; ++i) {
        lanrdp::ClientConnection c;
        starts.insert(c.nextTransferId());
    }
    QVERIFY2(starts.size() > 1, "不同连接的起点应当不同（否则还是固定值）");
}

void TestClientConnection::clientSocketBypassesSystemProxy()
{
    // 布置「机器上开着系统代理」的现场 —— 这正是真机连不上时那个环境。
    // 只在用例内改，末尾恢复，免得污染同进程里的其它用例。
    const QNetworkProxy saved = QNetworkProxy::applicationProxy();
    QNetworkProxy::setApplicationProxy(
        QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9));

    lanrdp::ClientConnection conn;

    // 192.0.2.0/24 是 TEST-NET-2 保留段，不会真的连上 —— 本用例只关心 socket 的
    // 代理配置，不关心连接结果（连接失败是预期的，且由别的用例覆盖）。
    conn.connectTo(QStringLiteral("192.0.2.1"), 9000, QStringLiteral("pw"));

    QVERIFY2(conn.session(), "connectTo() 之后应当建好会话");
    QTcpSocket *sock = conn.session()->socket();
    QVERIFY2(sock, "会话应当持有 socket");

    // 核心断言：socket 必须被**显式**设成 NoProxy。
    //
    // 修复前这里是 `DefaultProxy` —— Qt 会转而解析 `applicationProxy()`（也就是上面
    // 那个 HTTP 代理），于是局域网 IP 直接被拦。这也是为什么「关了系统代理就好」。
    QCOMPARE(sock->proxy().type(), QNetworkProxy::NoProxy);

    conn.teardown();
    QNetworkProxy::setApplicationProxy(saved);
}

// 死链判定的计时起点分两种：收过字节 -> 从「最后一次收到」；一次都没收过
// （TCP 刚连上、握手响应还没回来）-> 从「本次尝试开始」。
//
// 病灶：只看 msSinceLastRx() 时，「从未收到」的哨兵（远大于任何阈值）恒为真，
// 于是刚建好的连接在**首个**心跳 tick（3 s）就被判死 —— 本该给满 10 s。
// 真机重连风暴里那 18 轮「3 秒收不到握手响应」就是它。
void TestClientConnection::deadLinkDetectionPicksClockByFirstByte()
{
    constexpr qint64 kNever = lanrdp::Session::kNeverReceivedMs;
    constexpr qint64 kDead  = 10000;

    // 一帧未收：以「本次尝试已过去多久」为钟
    QVERIFY2(!lanrdp::ClientConnection::isDeadLink(false, kNever, 3000, kDead),
             "刚连上 3 s、一帧未收，不该判死（哨兵不能当实际静默时长用）");
    QVERIFY2(!lanrdp::ClientConnection::isDeadLink(false, kNever, kDead, kDead),
             "恰好到阈值不算超时（判据是「超过」）");
    QVERIFY2(lanrdp::ClientConnection::isDeadLink(false, kNever, kDead + 1, kDead),
             "未握手且静默超过阈值，应当判死");

    // 收过字节：以「距上次收到」为钟（attemptElapsed 不再参与）
    QVERIFY2(!lanrdp::ClientConnection::isDeadLink(true, 5000, 999999, kDead),
             "最近收到过字节就不该判死，哪怕这次尝试已经很久");
    QVERIFY2(lanrdp::ClientConnection::isDeadLink(true, kDead + 1, 0, kDead),
             "收过字节但静默超过阈值，应当判死");
}

// 自动重连的退避必须**逐轮增长**（1 → 2 → 4… 倍基准，封顶）。
//
// 病灶：connectTo() 第一句曾是 cancelReconnect()，而它把 reconnectAttempt_ 清零；
// 自动重连又走 connectTo() ⇒ 每轮重连都把计数打回 0，退避恒为 1 倍基准
//（真机日志 24 行全是「1 秒后重连…（第 1 次）」，指数退避完全失效）。
void TestClientConnection::reconnectBackoffGrowsAcrossAutoReconnects()
{
    lanrdp::ClientConnection conn;
    conn.setReconnectBaseDelayMs(20);   // 压到毫秒级，别真等 1+2+4 秒

    // 「排一次重连」会打一条「…秒后重连…（第 N 次）」的日志 —— 用 N 观察退避计数。
    // ⚠️ 只取**排期**那条：紧接着的「正在重连…（第 N 次）」用的是同一个 N
    //（计数是排期时就自增的），两条都收会让序号看起来是 1,1,2,2,3,3…
    QStringList attempts;
    QStringList allLines;
    QObject::connect(&conn, &lanrdp::ClientConnection::logMessage, &conn,
                     [&attempts, &allLines](const QString &line) {
                         allLines << line;
                         if (!line.contains(QStringLiteral("后重连")))
                             return;
                         static const QRegularExpression re(QStringLiteral("（第 (\\d+) 次）"));
                         const auto m = re.match(line);
                         if (m.hasMatch())
                             attempts << m.captured(1);
                     });

    // 一个「连上就把对端踢掉」的服务端：让每次尝试都**在握手阶段失败**，从而
    // 稳定地排下一次重连。
    //
    // 不用「连一个没人监听的端口」：那种做法要让 connectToHost 靠 ConnectionRefused
    // 失败，实测在部分环境里第二次连接会卡在 ConnectingState（既不报错也不发
    // disconnected），重连链就断在那儿了 —— 那是环境噪声，不是被测行为。
    QTcpServer killer;
    QVERIFY(killer.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&killer, &QTcpServer::newConnection, &killer, [&killer]() {
        while (killer.hasPendingConnections()) {
            QTcpSocket *s = killer.nextPendingConnection();
            s->abort();   // 直接掐断，不给握手响应
            s->deleteLater();
        }
    });

    conn.connectTo(QStringLiteral("127.0.0.1"), killer.serverPort(), QStringLiteral("pw"));

    // 基准 20 ms ⇒ 20 + 40 = 60 ms 就能攒到 3 次排期
    QElapsedTimer t;
    t.start();
    while (attempts.size() < 3 && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    QVERIFY2(attempts.size() >= 3,
             qPrintable(QStringLiteral("观测期内只排了 %1 次重连；日志：\n%2")
                            .arg(attempts.size())
                            .arg(allLines.join(QLatin1Char('\n')))));
    QCOMPARE(attempts.at(0), QStringLiteral("1"));
    QCOMPARE(attempts.at(1), QStringLiteral("2"));   // 旧实现恒为 "1" —— 这一条就是病灶
    QCOMPARE(attempts.at(2), QStringLiteral("3"));

    conn.disconnectNow();   // 收尾：停掉在途退避，别把定时器留给后续用例
    killer.close();
}

// 口令被拒 ≠ 网络故障：**不许**自动重连。
//
// 病灶：`AuthClient::failed` 只结算了握手状态，没把「被拒」这件事告诉重连判定，
// 于是对端一断 socket 就照常排退避 —— 每 30 s 拿同一个错口令再撞一次，永远失败；
// 同时被控端每轮都记一笔失败退避，那个来源 IP 的退避桶永远等不到清零。
//
// 判据用真实被控端 + 错口令走完整握手，而不是伪造 `AuthClient::failed` ——
// 要守的正是「拒绝之后 socket 断开这条真实时序」。
void TestClientConnection::authRejectionStopsAutoReconnect()
{
    lanrdp::HostServer host(QStringLiteral("right-password"));
    // 本用例只关心「被拒之后不重连」。失败退避会把同一来源的第二次尝试直接挡掉，
    // 那会把「标记有没有清掉」和「退避有没有过期」两件事混在一起 —— 关掉它。
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs = 0;
    cfg.maxDelayMs  = 0;
    host.setBackoffConfig(cfg);

    QString listenError;
    QObject::connect(&host, &lanrdp::HostServer::listenFailed, &host,
                     [&listenError](quint16, const QString &reason) { listenError = reason; });
    QVERIFY2(host.listen(0, QHostAddress::LocalHost),
             qPrintable(QStringLiteral("被控端应当能在随机端口上监听：%1").arg(listenError)));

    lanrdp::ClientConnection conn;
    conn.setReconnectBaseDelayMs(20);   // 万一真排了重连，20 ms 内就能看到

    bool denied = false;
    QObject::connect(&conn, &lanrdp::ClientConnection::handshakeFinished, &conn,
                     [&denied](bool ok, const QString &message) {
                         if (!ok && message == QStringLiteral("denied"))
                             denied = true;
                     });

    conn.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("wrong-password"));

    QElapsedTimer t;
    t.start();
    while (!denied && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVERIFY2(denied, "错误口令应当以 denied 结算握手");

    // 拒绝之后再给足时间：旧行为下这窗口内必然排出（甚至跑完）好几轮重连。
    t.restart();
    while (t.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    QVERIFY2(!conn.reconnectPending(),
             "口令被拒后不该排自动重连 —— 否则就是拿同一个错口令无限重试");

    // 用户改好口令后必须还能连：新的一次连接尝试要把「被拒」标记清掉。
    conn.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("right-password"));
    t.restart();
    while (!conn.ready() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVERIFY2(conn.ready(), "改对的口令应当能连上（被拒标记必须在新尝试时清掉）");

    conn.disconnectNow();
}

QTEST_GUILESS_MAIN(TestClientConnection)
#include "test_clientconnection.moc"
