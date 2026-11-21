#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <memory>

#include "net/ClientConnection.h"
#include "net/ClipboardSync.h"
#include "net/FileReceiver.h"
#include "net/HostServer.h"
#include "net/Session.h"
#include "protocol/Clipboard.h"
#include "protocol/FileTransfer.h"

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
    // 退役 socket 的迟到错误不许结算到新一轮尝试上（否则新连接永远 ready 不了）
    void errorFromRetiredSocketIsNotCurrent();
    // 连着的时候再连一次：旧 socket 的临终 disconnected 不许拆掉新连接
    void reconnectingWhileConnectedKeepsNewSession();
    // 一批文件必须串行发（FileSender 同一时刻只能传一个，撞上去会中止前一个）
    void sendFilesQueuesOneFileAtATime();
    // 队列里有一份发不出去就整批放弃（对端按清单凑批，缺一个就永远等不齐）
    void failedFileAbandonsTheRestOfTheBatch();
    // 剪贴板里的一批文件：先清单、再文件，两者必须指向同一批
    void sendClipboardFilesSendsManifestThenFiles();
    // 反向（被控端 → 控制端）：只有本端持着操作权时才把文件写到本机硬盘
    void fileFramesRequireTheInputToken();
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

// 退役 socket 的迟到错误不能被当成「当前连接」的错误。
//
// 病灶：错误分支读的是 session_->socket()，且会把 handshakeReported_ 置位。
// teardown() 之后旧 socket 并没有立刻消失（Session 走 deleteLater），它若在新一轮
// connectTo() 之后才报错，就会把新连接结算成失败 —— 之后真握手回来会被
// handshakeReported_ 挡在门外，ready 永远是 false，画面不来。
//
// 只钉判据本身：真实时序（旧 socket 恰好在重建之后的一个事件循环回合内报错）
// 没法在不给本类开测试钩子的前提下稳定复现，硬造一条就是不可靠用例。
void TestClientConnection::errorFromRetiredSocketIsNotCurrent()
{
    auto *current = new QTcpSocket;   // 所有权随即交给 Session
    auto *retired = new QTcpSocket;   // 模拟上一轮退役、尚未被 deleteLater 回收的 socket
    lanrdp::Session session(current, nullptr);

    QVERIFY2(lanrdp::ClientConnection::isCurrentSocket(&session, current),
             "当前会话持有的 socket 必须被判为当前 socket");
    QVERIFY2(!lanrdp::ClientConnection::isCurrentSocket(&session, retired),
             "退役 socket 的迟到错误不该结算到当前连接上");
    QVERIFY2(!lanrdp::ClientConnection::isCurrentSocket(nullptr, current),
             "会话已拆除时没有「当前 socket」，任何错误都不该结算");

    delete retired;   // current 随 session 析构回收
}

// 连着的时候再连一次（点【重连】、或对同一格重复发起连接）：新连接必须活下来。
//
// 病灶：socket 的 disconnected 处理没判归属。teardown() 把旧 Session 交给
// deleteLater，旧 socket 那一刻**仍连着**；下一轮事件循环回收它时，QTcpSocket
// 的析构会**同步**发 disconnected ⇒ onSocketDisconnected() 把 teardown() 刚建好
// 的新会话一并拆掉。界面上的表现就是「重连后画面一闪就断」。
//
// 判据用两条：新连接要真的就绪（关掉自动重连，否则重连兜底会把症状盖掉），
// 以及全程不许出现 connectionLost —— 这是我们主动换连接，不是「断了」。
void TestClientConnection::reconnectingWhileConnectedKeepsNewSession()
{
    lanrdp::HostServer host(QStringLiteral("secret"));
    QVERIFY2(host.listen(0, QHostAddress::LocalHost), "被控端应当能在随机端口上监听");

    lanrdp::ClientConnection conn;
    conn.setAutoReconnectEnabled(false);

    conn.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));

    QElapsedTimer t;
    t.start();
    while (!conn.ready() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVERIFY2(conn.ready(), "首次连接应当能建立");

    int lostCount = 0;
    QObject::connect(&conn, &lanrdp::ClientConnection::connectionLost, &conn,
                     [&lostCount]() { ++lostCount; });

    // 第二次连接：此刻旧 socket 还没被回收，仍在 ConnectedState。
    conn.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));

    // 强制处理 DeferredDelete —— processEvents() 不做这一步，而「旧 socket 析构」
    // 正是旧实现里新连接被拆的那一刻。
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    QVERIFY2(conn.session() != nullptr, "重连之后必须持有新的会话（旧会话的析构不许清空它）");

    t.restart();
    while (!conn.ready() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVERIFY2(conn.ready(), "重连之后新连接应当照常就绪");
    QCOMPARE(lostCount, 0);   // 旧实现这里为 1

    conn.disconnectNow();
}

// 一次交出去三个文件，必须是「一个传完再传下一个」。
//
// FileSender 的 start() 在状态仍是 Sending 时会先 conclude(false) 把前一个中止掉，
// 所以从前连着调三次 sendFile 只会送达最后一个（前两个各留一条「已中止」日志、
// 接收侧半截文件被删）。剪贴板复制文件正好是「一次三个」，必须走队列。
void TestClientConnection::sendFilesQueuesOneFileAtATime()
{
    lanrdp::HostServer host(QStringLiteral("secret"));
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs = 0;
    cfg.maxDelayMs  = 0;
    host.setBackoffConfig(cfg);
    QVERIFY(host.listen(0, QHostAddress::LocalHost));

    QTemporaryDir recvDir;
    QVERIFY(recvDir.isValid());

    // 接收侧用真的 FileReceiver（生产拓扑：一个实例、按来源分别记账）
    auto receiver = std::make_unique<lanrdp::FileReceiver>(recvDir.path());
    QStringList landed;
    QObject::connect(receiver.get(), &lanrdp::FileReceiver::finished, receiver.get(),
                     [&landed](bool ok, const QString &path, const QString &) {
                         if (ok)
                             landed.append(path);
                     });
    QObject::connect(&host, &lanrdp::HostServer::clientAuthenticated, &host,
                     [&receiver](lanrdp::Session *session) {
                         QObject::connect(session, &lanrdp::Session::frameReceived,
                                          receiver.get(),
                                          [&receiver, session](const lanrdp::Frame &frame) {
                                              receiver->onFrame(session, frame);
                                          });
                     });

    lanrdp::ClientConnection conn;
    QStringList started;
    QObject::connect(&conn, &lanrdp::ClientConnection::fileStarted, &conn,
                     [&started](const QString &name, quint64) { started.append(name); });

    conn.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QElapsedTimer t;
    t.start();
    while (!conn.ready() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVERIFY2(conn.ready(), "应当能连上测试用被控端");

    QTemporaryDir sendDir;
    QVERIFY(sendDir.isValid());
    QStringList paths;
    for (const char *name : {"a.txt", "b.txt", "c.txt"}) {
        const QString path = QDir(sendDir.path()).absoluteFilePath(QLatin1String(name));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("payload-") + name);
        file.close();
        paths.append(path);
    }

    QVERIFY(conn.sendFiles(paths));
    QTRY_COMPARE_WITH_TIMEOUT(landed.size(), 3, 15000);

    // 顺序 = 提交顺序：串行发送的唯一可观测证据
    QStringList landedNames;
    for (const QString &path : landed)
        landedNames.append(QFileInfo(path).fileName());
    QCOMPARE(landedNames,
             (QStringList{QStringLiteral("a.txt"), QStringLiteral("b.txt"),
                          QStringLiteral("c.txt")}));
    QCOMPARE(started, landedNames);

    conn.disconnectNow();
}

// 队列里有一份发不出去就整批放弃，不接受「能发几个发几个」。
//
// 剪贴板这批是对端按清单**凑齐**才写进剪贴板的：少一个就永远凑不齐，
// 已经落盘的几个只能在超时后连同清单一起作废（对方白白多收文件）。
// 所以 start() 一失败就清队列、别再往下发。
void TestClientConnection::failedFileAbandonsTheRestOfTheBatch()
{
    lanrdp::HostServer host(QStringLiteral("secret"));
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs = 0;
    cfg.maxDelayMs  = 0;
    host.setBackoffConfig(cfg);
    QVERIFY(host.listen(0, QHostAddress::LocalHost));

    QTemporaryDir recvDir;
    QVERIFY(recvDir.isValid());

    auto receiver = std::make_unique<lanrdp::FileReceiver>(recvDir.path());
    QStringList landed;
    QObject::connect(receiver.get(), &lanrdp::FileReceiver::finished, receiver.get(),
                     [&landed](bool ok, const QString &path, const QString &) {
                         if (ok)
                             landed.append(QFileInfo(path).fileName());
                     });
    QObject::connect(&host, &lanrdp::HostServer::clientAuthenticated, &host,
                     [&receiver](lanrdp::Session *session) {
                         QObject::connect(session, &lanrdp::Session::frameReceived,
                                          receiver.get(),
                                          [&receiver, session](const lanrdp::Frame &frame) {
                                              receiver->onFrame(session, frame);
                                          });
                     });

    lanrdp::ClientConnection conn;
    QStringList started;
    QStringList logs;
    QObject::connect(&conn, &lanrdp::ClientConnection::fileStarted, &conn,
                     [&started](const QString &name, quint64) { started.append(name); });
    QObject::connect(&conn, &lanrdp::ClientConnection::logMessage, &conn,
                     [&logs](const QString &message) { logs.append(message); });

    conn.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QElapsedTimer t;
    t.start();
    while (!conn.ready() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVERIFY2(conn.ready(), "应当能连上测试用被控端");

    QTemporaryDir sendDir;
    QVERIFY(sendDir.isValid());
    auto createFile = [&sendDir](const QString &name) {
        const QString path = QDir(sendDir.path()).absoluteFilePath(name);
        QFile         file(path);
        if (!file.open(QIODevice::WriteOnly))
            return QString();
        file.write(QByteArrayLiteral("payload-") + name.toUtf8());
        return path;
    };
    const QString first = createFile(QStringLiteral("a.txt"));
    const QString third = createFile(QStringLiteral("c.txt"));
    QVERIFY(!first.isEmpty());
    QVERIFY(!third.isEmpty());

    // 中间那个不存在 ⇒ start() 立即失败（空文件同理）
    const QString missing = QDir(sendDir.path()).absoluteFilePath(QStringLiteral("missing.txt"));
    QVERIFY(!QFileInfo::exists(missing));

    QVERIFY(conn.sendFiles(QStringList{first, missing, third}));

    // 先等接收侧真的落了第一份 —— 发送端的 finished 早于对端写盘，
    // 只等日志会在接收侧还没处理完时就断言（实测 landed 仍是 0）。
    QTRY_COMPARE_WITH_TIMEOUT(landed.size(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !logs.filter(QStringLiteral("已放弃队列中剩余的")).isEmpty(), 5000);
    QVERIFY2(logs.filter(QStringLiteral("已放弃队列中剩余的 1 个文件")).size() == 1,
             qPrintable(logs.join(QLatin1Char('\n'))));

    // 第一份照常送达；第三份一次都不该被发出（连 fileStarted 都不该有）
    QCOMPARE(landed, QStringList{QStringLiteral("a.txt")});
    QCOMPARE(started, QStringList{QStringLiteral("a.txt")});

    // 再等一拍，确认没有「悄悄补发」
    QTest::qWait(300);
    QCOMPARE(landed, QStringList{QStringLiteral("a.txt")});

    conn.disconnectNow();
}

// 剪贴板里的一批文件：**清单先走、文件随后**，而且清单里的名字与真正落盘的那批一致。
// 清单早一个、晚一个，接收侧就会一直等那几个不来的文件（最后整批放弃）。
void TestClientConnection::sendClipboardFilesSendsManifestThenFiles()
{
    lanrdp::HostServer host(QStringLiteral("secret"));
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs = 0;
    cfg.maxDelayMs  = 0;
    host.setBackoffConfig(cfg);
    QVERIFY(host.listen(0, QHostAddress::LocalHost));

    QTemporaryDir recvDir;
    QVERIFY(recvDir.isValid());

    auto receiver = std::make_unique<lanrdp::FileReceiver>(recvDir.path());
    QStringList landed;
    QObject::connect(receiver.get(), &lanrdp::FileReceiver::finished, receiver.get(),
                     [&landed](bool ok, const QString &path, const QString &) {
                         if (ok)
                             landed.append(path);
                     });

    // 接收侧的剪贴板同步器（不装端口：本用例只关心它认出清单这件事）
    auto clipboard = std::make_unique<lanrdp::ClipboardSync>();
    QStringList manifestNames;
    bool        manifestCameFirst = false;
    QObject::connect(clipboard.get(), &lanrdp::ClipboardSync::filesIncoming, clipboard.get(),
                     [&manifestNames, &manifestCameFirst, &landed](const QStringList &names) {
                         manifestNames    = names;
                         manifestCameFirst = landed.isEmpty();   // 清单必须先于文件
                     });

    QObject::connect(&host, &lanrdp::HostServer::clientAuthenticated, &host,
                     [&receiver, &clipboard](lanrdp::Session *session) {
                         QObject::connect(session, &lanrdp::Session::frameReceived, receiver.get(),
                                          [&receiver, session](const lanrdp::Frame &frame) {
                                              receiver->onFrame(session, frame);
                                          });
                         QObject::connect(session, &lanrdp::Session::frameReceived, clipboard.get(),
                                          [&clipboard](const lanrdp::Frame &frame) {
                                              clipboard->handleFrame(frame);
                                          });
                     });

    lanrdp::ClientConnection conn;
    QStringList logs;
    QObject::connect(&conn, &lanrdp::ClientConnection::logMessage, &conn,
                     [&logs](const QString &line) { logs.append(line); });

    conn.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QElapsedTimer t;
    t.start();
    while (!conn.ready() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVERIFY2(conn.ready(), "应当能连上测试用被控端");

    QTemporaryDir sendDir;
    QVERIFY(sendDir.isValid());
    QStringList paths;
    for (const char *name : {"甲.txt", "乙.txt"}) {
        // 源码是 UTF-8，字面量按 UTF-8 还原（QLatin1String 会把字节当 Latin-1 读坏）
        const QString path = QDir(sendDir.path()).absoluteFilePath(QString::fromUtf8(name));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("payload-") + name);
        file.close();
        paths.append(path);
    }

    QVERIFY(conn.sendClipboardFiles(paths));
    QTRY_COMPARE_WITH_TIMEOUT(landed.size(), 2, 15000);

    QCOMPARE(manifestNames, (QStringList{QStringLiteral("甲.txt"), QStringLiteral("乙.txt")}));
    QVERIFY2(manifestCameFirst, "清单必须先到：接收侧靠它决定「凑几个才写剪贴板」");

    // 超过上限：一个都不发，而且原因要落到日志里（按钮没反应总得有个说法）
    QStringList tooMany;
    for (int i = 0; i < lanrdp::kClipboardMaxFiles + 1; ++i)
        tooMany.append(QDir(sendDir.path()).absoluteFilePath(QStringLiteral("x%1.txt").arg(i)));
    QVERIFY(!conn.sendClipboardFiles(tooMany));
    QVERIFY2(!logs.isEmpty() && logs.last().contains(QStringLiteral("64")),
             qPrintable(logs.isEmpty() ? QStringLiteral("没有日志") : logs.last()));

    conn.disconnectNow();
}

// 反向传文件（被控端 → 控制端）：本端必须**持着操作权**才收。
//
// 门放在 ClientConnection 而不是只放在发送端：对端可以被换成任何东西，
// 接收端自己守才算数。正常情况下两道门同开同闭（被控端也只在有人持令牌时发），
// 所以这里造的正是「门不一样」的那一刻。
void TestClientConnection::fileFramesRequireTheInputToken()
{
    lanrdp::HostServer host(QStringLiteral("secret"));
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs = 0;
    cfg.maxDelayMs  = 0;
    host.setBackoffConfig(cfg);
    QVERIFY(host.listen(0, QHostAddress::LocalHost));

    lanrdp::Session *peer = nullptr;
    QObject::connect(&host, &lanrdp::HostServer::clientAuthenticated, &host,
                     [&peer](lanrdp::Session *session) { peer = session; });

    QTemporaryDir recvDir;
    QVERIFY(recvDir.isValid());

    lanrdp::ClientConnection conn;
    conn.setReceiveDir(recvDir.path());
    QStringList landed;
    QObject::connect(&conn, &lanrdp::ClientConnection::fileReceived, &conn,
                     [&landed](bool ok, const QString &path, const QString &) {
                         if (ok)
                             landed.append(path);
                     });

    conn.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QTRY_VERIFY(conn.ready());
    QTRY_VERIFY(peer != nullptr);
    QVERIFY2(!conn.hasToken(), "被控端没发令牌时本端不该自认为持有操作权");

    // 一小份完整的文件（HEADER + 一块 CHUNK + END），够走完落盘与 sha 校验。
    const QByteArray body = QByteArrayLiteral("reverse-direction-payload");
    const auto sendWholeFile = [&body](lanrdp::Session *s, const QString &name) {
        lanrdp::FileHeader header;
        header.transferId = 42;
        header.name       = name;
        header.totalSize  = quint64(body.size());
        s->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::FileHeader),
                     lanrdp::encodeFileHeader(header));

        lanrdp::FileChunk chunk;
        chunk.transferId = 42;
        chunk.offset     = 0;
        chunk.data       = body;
        s->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::FileChunk),
                     lanrdp::encodeFileChunk(chunk));

        lanrdp::FileEnd end;
        end.transferId = 42;
        end.bytesSent  = quint64(body.size());
        end.sha256     = QCryptographicHash::hash(body, QCryptographicHash::Sha256);
        s->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::FileEnd),
                     lanrdp::encodeFileEnd(end));
    };

    // ① 没有操作权：整份文件都不该落盘，也不该留下任何痕迹
    sendWholeFile(peer, QStringLiteral("不给.txt"));
    QTest::qWait(600);
    QVERIFY2(landed.isEmpty(), "没持操作权时不该往本机硬盘写文件");
    QCOMPARE(QDir(recvDir.path()).entryList(QDir::Files).size(), 0);

    // ② 拿到操作权：同样的帧就要落地、内容与 sha 都要对上
    lanrdp::Frame grant;
    grant.type = static_cast<std::uint8_t>(lanrdp::FrameType::TokenGrant);
    peer->sendFrame(grant);
    QTRY_VERIFY(conn.hasToken());

    sendWholeFile(peer, QStringLiteral("给我.txt"));
    QTRY_COMPARE_WITH_TIMEOUT(landed.size(), 1, 15000);

    QCOMPARE(QFileInfo(landed.first()).fileName(), QStringLiteral("给我.txt"));
    QFile back(landed.first());
    QVERIFY(back.open(QIODevice::ReadOnly));
    QCOMPARE(back.readAll(), body);

    conn.disconnectNow();
}

QTEST_GUILESS_MAIN(TestClientConnection)
#include "test_clientconnection.moc"
