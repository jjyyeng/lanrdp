#include <QtTest/QtTest>

#include <QVector>
#include <QTcpServer>
#include <QTcpSocket>

#include "net/Session.h"
#include "protocol/Frame.h"

// Session 帧分帧（loopback 收发一帧 / 粘包拆分）
// Session 记录「最后一次收到字节」的时刻（心跳 / 死链判定的基准）
class TestSession : public QObject
{
    Q_OBJECT

private slots:
    void loopbackDeliversOneFrame();
    void splitsTwoFramesInOneWrite();
    void msSinceLastRxStartsAsNever();
    void msSinceLastRxResetsOnFrame();
    void pendingBytesStartsAtZero();
    void pendingBytesReflectsUnflushedWrites();
    // 畸形帧长必须断开，且接收缓冲不得无界增长
    void absurdPayloadLenDisconnectsAndBoundsBuffer();
    // 分帧必须支持「一帧跨多次 readyRead」（TCP 真实会这样切）
    void frameSplitAcrossMultipleReadyReads();
    void payloadSplitAcrossManyReadyReads();
    // 一次 readyRead 内大量小帧必须全部派发且不丢帧
    void manySmallFramesInOneWriteAllDispatched();
};

void TestSession::loopbackDeliversOneFrame()
{
    QTcpServer server;
    QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
             qPrintable(QStringLiteral("listen 失败: %1").arg(server.errorString())));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);

    QVector<lanrdp::Frame> received;
    QObject::connect(&session, &lanrdp::Session::frameReceived,
                     [&received](const lanrdp::Frame &f) { received.append(f); });

    // 客户端发一帧 HANDSHAKE_OK(type=2)，空 payload
    lanrdp::Frame ok;
    ok.type = 0x02;
    client.write(ok.serialize());
    QVERIFY(client.waitForBytesWritten(3000));

    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received.at(0).type, quint8(0x02));
    QCOMPARE(received.at(0).payload, QByteArray());
}

void TestSession::splitsTwoFramesInOneWrite()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);

    QVector<lanrdp::Frame> received;
    QObject::connect(&session, &lanrdp::Session::frameReceived,
                     [&received](const lanrdp::Frame &f) { received.append(f); });

    // 一次写入两帧（粘包）：REQ(payload="hi") + OK(空)
    lanrdp::Frame a;
    a.type    = 0x01;
    a.payload = QByteArrayLiteral("hi");
    lanrdp::Frame b;
    b.type = 0x02;

    QByteArray all = a.serialize();
    all.append(b.serialize());
    client.write(all);
    QVERIFY(client.waitForBytesWritten(3000));

    QTRY_COMPARE(received.size(), 2);
    QCOMPARE(received.at(0).type, quint8(0x01));
    QCOMPARE(received.at(0).payload, QByteArrayLiteral("hi"));
    QCOMPARE(received.at(1).type, quint8(0x02));
    QCOMPARE(received.at(1).payload, QByteArray());
}

// 新建 Session 一帧都没收到时，必须返回「从未收到」的哨兵值。
// 心跳的死链判定就靠这个值：哨兵远大于任何阈值 -> 立刻可判死。
// （反过来说，谁拿到一个 Session 后想用它做「多久没收到东西」的判断，
// 都得能区分「刚建、还没收过」和「收过、但久了」，否则刚连上就会被误杀。）
void TestSession::msSinceLastRxStartsAsNever()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);

    QCOMPARE(session.msSinceLastRx(), lanrdp::Session::kNeverReceivedMs);
    QVERIFY(session.msSinceLastRx() > 10000);  // 远大于任何现实的死链阈值

    // 哨兵本身只能说明「从未收到」—— 判死链的调用方必须靠 hasReceived() 挑计时起点，
    // 不能拿这个值去和阈值比大小（一比就把刚连上的连接判死）。
    QVERIFY2(!session.hasReceived(), "还没收到任何字节时 hasReceived() 必须为假");
}

// 收到任何字节即刷新计时；之后随时间单调增长。
void TestSession::msSinceLastRxResetsOnFrame()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);

    QVector<lanrdp::Frame> received;
    QObject::connect(&session, &lanrdp::Session::frameReceived,
                     [&received](const lanrdp::Frame &f) { received.append(f); });

    lanrdp::Frame ok;
    ok.type = 0x02;
    client.write(ok.serialize());
    QVERIFY(client.waitForBytesWritten(3000));

    QTRY_COMPARE(received.size(), 1);
    QVERIFY2(session.msSinceLastRx() < 1000, "收到帧后应当立刻回落");
    QVERIFY(session.msSinceLastRx() != lanrdp::Session::kNeverReceivedMs);
    QVERIFY2(session.hasReceived(), "收到过字节后 hasReceived() 必须为真");

    QTest::qWait(120);
    QVERIFY2(session.msSinceLastRx() >= 100, "计时应当随时间前进");
}

// pendingBytes = socket 待写字节（自适应码率的「发送队列积压」信号）。
// 刚建好、什么都没写时必须是 0。
void TestSession::pendingBytesStartsAtZero()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);
    QCOMPARE(session.pendingBytes(), 0);
}

// 写一个远超 socket 缓冲的大帧、对端不读时，待写字节应 > 0。
// 这是「链路慢了、发送跟不上」在发送侧的直接量度——自适应码率就靠它判「卡了」。
void TestSession::pendingBytesReflectsUnflushedWrites()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);
    QCOMPARE(session.pendingBytes(), 0);

    // 8MB 远超 loopback 的 socket 发送缓冲 + 内核缓冲，且对端（client）不读
    lanrdp::Frame big;
    big.type    = 0x02;
    big.payload = QByteArray(8 * 1024 * 1024, 'x');
    session.sendFrame(big);

    QVERIFY2(session.pendingBytes() > 0, "对端不读、写入大帧后待写字节应为正");
}

// ---- 畸形帧长 -> 断开，缓冲不得无界增长 ----

// 未认证对端发一个「头合法 + 声明 0xFFFFFFFF + 缓慢滴流」即可把 rxbuf_ 撑爆。
// 修复后：解析器判 Invalid -> Session 清缓冲 + 主动断开。
void TestSession::absurdPayloadLenDisconnectsAndBoundsBuffer()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);

    // 手动拼一个声明 0xFFFFFFFF 的帧头
    QByteArray evil;
    evil.append(char('L'));
    evil.append(char('R'));
    evil.append(char(lanrdp::kVersion));
    evil.append(char(0x05));
    evil.append(char(0xFF));
    evil.append(char(0xFF));
    evil.append(char(0xFF));
    evil.append(char(0xFF));

    client.write(evil);
    QVERIFY(client.waitForBytesWritten(3000));

    // Session 必须主动断开（不是永久等待）
    QTRY_VERIFY_WITH_TIMEOUT(serverSide->state() != QAbstractSocket::ConnectedState, 3000);
}

// ---- 一帧跨多次 readyRead ----

// TCP 会把一帧切成任意片段到达（报告指出：原有测试只写「两个完整帧」，从未覆盖此路径）。
void TestSession::frameSplitAcrossMultipleReadyReads()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);

    QVector<lanrdp::Frame> received;
    QObject::connect(&session, &lanrdp::Session::frameReceived,
                     [&received](const lanrdp::Frame &f) { received.append(f); });

    lanrdp::Frame f;
    f.type    = 0x01;
    f.payload = QByteArrayLiteral("hello-split");
    const QByteArray wire = f.serialize();

    // 逐字节发送：头被切碎、载荷也被切碎
    for (char c : wire) {
        client.write(QByteArray(1, c));
        QVERIFY(client.waitForBytesWritten(3000));
        QCoreApplication::processEvents();
    }

    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received.at(0).type, quint8(0x01));
    QCOMPARE(received.at(0).payload, QByteArrayLiteral("hello-split"));
}

void TestSession::payloadSplitAcrossManyReadyReads()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);

    QVector<lanrdp::Frame> received;
    QObject::connect(&session, &lanrdp::Session::frameReceived,
                     [&received](const lanrdp::Frame &f) { received.append(f); });

    // 1KB 载荷，分 16 次各 64 字节发
    QByteArray payload(1024, 'z');
    lanrdp::Frame f;
    f.type = 0x04;
    f.payload = payload;
    const QByteArray wire = f.serialize();

    for (int off = 0; off < wire.size(); off += 64) {
        client.write(wire.mid(off, 64));
        QVERIFY(client.waitForBytesWritten(3000));
        QCoreApplication::processEvents();
    }

    QTRY_COMPARE(received.size(), 1);
    QCOMPARE(received.at(0).payload, payload);
}

// ---- 一次 readyRead 内大量小帧全部派发（不丢帧、不无限循环）----

// 这个用例守护「分帧循环预算」：加了预算但必须仍能派发完
// 一次到达的全部帧（预算只能分批，不能丢帧）。
void TestSession::manySmallFramesInOneWriteAllDispatched()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);

    QVector<lanrdp::Frame> received;
    QObject::connect(&session, &lanrdp::Session::frameReceived,
                     [&received](const lanrdp::Frame &f) { received.append(f); });

    // 200 个最小帧（空载荷，仅 8 字节头）一次性写入
    constexpr int kCount = 200;
    QByteArray all;
    for (int i = 0; i < kCount; ++i) {
        lanrdp::Frame f;
        f.type = 0x02;
        all.append(f.serialize());
    }
    client.write(all);
    QVERIFY(client.waitForBytesWritten(3000));

    QTRY_COMPARE(received.size(), kCount);
}

QTEST_GUILESS_MAIN(TestSession)
#include "test_session.moc"
