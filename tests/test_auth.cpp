#include <QtTest/QtTest>

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>

#include "net/Auth.h"
#include "net/AuthBackoff.h"
#include "net/Session.h"
#include "protocol/Frame.h"

// 鉴权握手（正确口令 -> HANDSHAKE_OK / 错误口令 -> HANDSHAKE_DENY）
// 以及按来源 IP 分桶的登录失败退避。
class TestAuth : public QObject
{
    Q_OBJECT

private slots:
    void hashIsDeterministicAndSized();
    void verifyRequestMatchesAndRejects();
    void loopbackHandshakeAcceptsCorrectPassword();
    void loopbackHandshakeRejectsWrongPassword();

    // 挑战-响应：nonce 新鲜度 / HMAC 构造 / 重放必须失败
    void challengeNonceIsFreshPerConnection();
    void responseIsHmacOfPasswordAndNonce();
    void replayingOldResponseIsRejected();
    void duplicateResponseDoesNotAuthenticateTwice();

    // 登录失败退避（纯逻辑，注入时钟，不真等）
    void backoffAllowsFirstAttempt();
    void backoffDoublesAndCaps();
    void backoffIsBucketedBySource();
    void backoffResetsAfterQuietWindow();
    void backoffSuccessClearsCounter();
};

// sha256 摘要：定长 32 字节、同输入同输出、异输入异输出
void TestAuth::hashIsDeterministicAndSized()
{
    const QByteArray a = lanrdp::auth::hashPassword(QStringLiteral("s3cret"));
    const QByteArray b = lanrdp::auth::hashPassword(QStringLiteral("s3cret"));
    const QByteArray c = lanrdp::auth::hashPassword(QStringLiteral("other"));

    QCOMPARE(a.size(), 32);
    QCOMPARE(a, b);
    QVERIFY(a != c);
}

// 纯校验逻辑：请求载荷与期望口令比对
void TestAuth::verifyRequestMatchesAndRejects()
{
    const QString expected = QStringLiteral("s3cret");
    const QByteArray good   = lanrdp::auth::hashPassword(expected);
    const QByteArray bad    = lanrdp::auth::hashPassword(QStringLiteral("wrong"));

    QVERIFY(lanrdp::auth::verifyRequest(good, expected));
    QVERIFY(!lanrdp::auth::verifyRequest(bad, expected));
    QVERIFY(!lanrdp::auth::verifyRequest(QByteArray(), expected));
}

void TestAuth::loopbackHandshakeAcceptsCorrectPassword()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket *clientSock = new QTcpSocket();
    clientSock->connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(clientSock->waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSock = server.nextPendingConnection();
    QVERIFY(serverSock != nullptr);

    auto *serverSession = new lanrdp::Session(serverSock);
    auto *clientSession = new lanrdp::Session(clientSock);

    const QString password = QStringLiteral("s3cret");

    // 两个 Session 都必须先建好再接握手器 —— 现在 AuthServer::attach() 会**立刻**
    // 发 CHALLENGE，若客户端 Session 还没建、readyRead 已经烧掉，那一帧就丢了。
    lanrdp::AuthServer authServer(password);
    authServer.attach(serverSession);

    bool serverOk = false;
    bool serverDenied = false;
    QObject::connect(&authServer, &lanrdp::AuthServer::authenticated,
                     [&serverOk]() { serverOk = true; });
    QObject::connect(&authServer, &lanrdp::AuthServer::rejected,
                     [&serverDenied]() { serverDenied = true; });

    lanrdp::AuthClient authClient(clientSession, password);
    bool clientOk = false;
    bool clientFailed = false;
    QObject::connect(&authClient, &lanrdp::AuthClient::succeeded,
                     [&clientOk]() { clientOk = true; });
    QObject::connect(&authClient, &lanrdp::AuthClient::failed,
                     [&clientFailed]() { clientFailed = true; });

    authClient.start();

    QTRY_VERIFY_WITH_TIMEOUT(clientOk, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(serverOk, 3000);
    QVERIFY(!clientFailed);
    QVERIFY(!serverDenied);
}

void TestAuth::loopbackHandshakeRejectsWrongPassword()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket *clientSock = new QTcpSocket();
    clientSock->connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(clientSock->waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSock = server.nextPendingConnection();
    QVERIFY(serverSock != nullptr);

    auto *serverSession = new lanrdp::Session(serverSock);
    auto *clientSession = new lanrdp::Session(clientSock);

    lanrdp::AuthServer authServer(QStringLiteral("right-password"));
    authServer.attach(serverSession);

    bool serverDenied = false;
    QObject::connect(&authServer, &lanrdp::AuthServer::rejected,
                     [&serverDenied]() { serverDenied = true; });

    lanrdp::AuthClient authClient(clientSession, QStringLiteral("wrong-password"));
    bool clientFailed = false;
    QObject::connect(&authClient, &lanrdp::AuthClient::failed,
                     [&clientFailed]() { clientFailed = true; });

    authClient.start();

    QTRY_VERIFY_WITH_TIMEOUT(clientFailed, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(serverDenied, 3000);
}

// ---------------------------------------------------------------------------
// 挑战-响应
// ---------------------------------------------------------------------------

// nonce 必须每个连接都新 —— 否则攻击者抓到一对 (nonce, response) 就能重放。
// 这里直接查非空 + 定长 + 不重复（同进程内连续 8 次取样）。
void TestAuth::challengeNonceIsFreshPerConnection()
{
    QSet<QByteArray> seen;
    for (int i = 0; i < 8; ++i) {
        const QByteArray n = lanrdp::auth::makeNonce();
        QCOMPARE(n.size(), lanrdp::auth::kNonceSize);
        QVERIFY2(!seen.contains(n), "nonce 重复 —— 重放防护失效");
        seen.insert(n);
    }
}

// 响应 = HMAC-SHA256(key = 口令 UTF-8, msg = nonce)。
// 期望值用 QMessageAuthenticationCode 独立算一遍（不调用被测函数），
// 否则就是「自己实现对自己」，什么都证明不了。
void TestAuth::responseIsHmacOfPasswordAndNonce()
{
    const QString    password = QStringLiteral("s3cret");
    const QByteArray nonce =
        QByteArray::fromHex("00112233445566778899aabbccddeeff"
                            "0123456789abcdef0123456789abcdef");
    const QByteArray expected =
        QMessageAuthenticationCode::hash(nonce, password.toUtf8(), QCryptographicHash::Sha256);

    const QByteArray actual = lanrdp::auth::computeResponse(password, nonce);

    QCOMPARE(actual, expected);
    QVERIFY(lanrdp::auth::verifyResponse(expected, password, nonce));
    QVERIFY(!lanrdp::auth::verifyResponse(QByteArray(32, '\0'), password, nonce));
    QVERIFY(!lanrdp::auth::verifyResponse(expected, QStringLiteral("wrong"), nonce));
    // 同一个正确响应配一个**新 nonce** 必须失效 —— 这正是重放被挡住的地方。
    QVERIFY(!lanrdp::auth::verifyResponse(expected, password, lanrdp::auth::makeNonce()));
}

// 端到端重放：正常握手一次，录下服务端发出的 challenge；
// 然后第二条连接上**原样重放**第一轮那个响应 —— 服务端必须拒绝。
//
// 客户端响应帧由测试自己构造（走 Session 直接发帧），不依赖 AuthClient 暴露中间值。
void TestAuth::replayingOldResponseIsRejected()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const QString password = QStringLiteral("s3cret");

    QByteArray firstChallenge;
    QByteArray firstResponse;

    // ---- 第一轮：真实握手，并从服务端视角录下 challenge。
    {
        QTcpSocket *clientSock = new QTcpSocket();
        clientSock->connectToHost(QHostAddress::LocalHost, server.serverPort());
        QVERIFY(clientSock->waitForConnected(3000));
        QVERIFY(server.waitForNewConnection(3000));

        auto *serverSession = new lanrdp::Session(server.nextPendingConnection());
        auto *clientSession = new lanrdp::Session(clientSock);

        // CHALLENGE 是服务端**发出**的 → 只能在客户端 Session 上听到。
        QObject::connect(clientSession, &lanrdp::Session::frameReceived,
                         [&firstChallenge](const lanrdp::Frame &f) {
                             if (f.type
                                 == static_cast<std::uint8_t>(
                                     lanrdp::FrameType::HandshakeChallenge))
                                 firstChallenge = f.payload;
                         });

        lanrdp::AuthServer authServer(password);
        authServer.attach(serverSession);

        lanrdp::AuthClient authClient(clientSession, password);
        bool               ok = false;
        QObject::connect(&authClient, &lanrdp::AuthClient::succeeded, [&ok]() { ok = true; });
        authClient.start();
        QTRY_VERIFY_WITH_TIMEOUT(ok, 3000);
        QCOMPARE(firstChallenge.size(), lanrdp::auth::kNonceSize);

        firstResponse = lanrdp::auth::computeResponse(password, firstChallenge);
    }

    // ---- 第二轮：第二条连接，不用 AuthClient，手工重放上一轮的响应。
    {
        QTcpSocket *clientSock = new QTcpSocket();
        clientSock->connectToHost(QHostAddress::LocalHost, server.serverPort());
        QVERIFY(clientSock->waitForConnected(3000));
        QVERIFY(server.waitForNewConnection(3000));

        auto *serverSession = new lanrdp::Session(server.nextPendingConnection());
        auto *clientSession = new lanrdp::Session(clientSock);

        lanrdp::AuthServer authServer(password);
        authServer.attach(serverSession);

        bool denied = false;
        bool authed = false;
        QObject::connect(&authServer, &lanrdp::AuthServer::rejected, [&denied]() { denied = true; });
        QObject::connect(&authServer, &lanrdp::AuthServer::authenticated,
                         [&authed]() { authed = true; });

        // 等服务端把 CHALLENGE 发过来（协议要求服务端先发），再重放旧响应。
        bool gotChallenge = false;
        QObject::connect(clientSession, &lanrdp::Session::frameReceived,
                         [&gotChallenge, clientSession, &firstResponse](const lanrdp::Frame &f) {
                             if (f.type
                                 != static_cast<std::uint8_t>(
                                     lanrdp::FrameType::HandshakeChallenge))
                                 return;
                             gotChallenge = true;
                             // 重放：直接甩上一轮那个 32 B 响应。
                             clientSession->sendFrame(
                                 static_cast<std::uint8_t>(lanrdp::FrameType::HandshakeResponse),
                                 firstResponse);
                         });

        QTRY_VERIFY_WITH_TIMEOUT(gotChallenge, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(denied, 3000);
        QVERIFY2(!authed, "重放旧响应竟然通过了 —— 挑战-响应没起作用");
    }
}

// 同一条连接上重复发响应**不得**被认证两次。
//
// 旧协议下这条洞是「重复发 HANDSHAKE_REQ → 双入池 → 输入被注入两次」；
// 挑战-响应的引入不改变它的存在（恶意对端手工重发即可），但也不能把它弄得更糟。
// 这里钉住：AuthServer 在给出结论后必须落闩，后续响应一律忽略。
void TestAuth::duplicateResponseDoesNotAuthenticateTwice()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const QString password = QStringLiteral("s3cret");

    QTcpSocket *clientSock = new QTcpSocket();
    clientSock->connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(clientSock->waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    auto *serverSession = new lanrdp::Session(server.nextPendingConnection());
    auto *clientSession = new lanrdp::Session(clientSock);

    lanrdp::AuthServer authServer(password);
    authServer.attach(serverSession);

    int authed = 0;
    int denied = 0;
    QObject::connect(&authServer, &lanrdp::AuthServer::authenticated, [&authed]() { ++authed; });
    QObject::connect(&authServer, &lanrdp::AuthServer::rejected, [&denied]() { ++denied; });

    // 手工握手：收 CHALLENGE → 连回**两份相同**的响应。
    bool replied = false;
    QObject::connect(clientSession, &lanrdp::Session::frameReceived,
                     [&](const lanrdp::Frame &f) {
                         if (f.type
                             != static_cast<std::uint8_t>(lanrdp::FrameType::HandshakeChallenge))
                             return;
                         const QByteArray resp = lanrdp::auth::computeResponse(password, f.payload);
                         const auto       type =
                             static_cast<std::uint8_t>(lanrdp::FrameType::HandshakeResponse);
                         clientSession->sendFrame(type, resp);
                         clientSession->sendFrame(type, resp);   // 重复一份
                         replied = true;
                     });

    QTRY_VERIFY_WITH_TIMEOUT(replied, 3000);
    QTest::qWait(300);   // 给第二份响应足够的时间到达并被处理

    QCOMPARE(authed, 1);
    QCOMPARE(denied, 0);
}

// ---------------------------------------------------------------------------
// 登录失败退避（纯逻辑）
// ---------------------------------------------------------------------------

void TestAuth::backoffAllowsFirstAttempt()
{
    lanrdp::AuthBackoff backoff;
    QVERIFY(backoff.isAllowed(QStringLiteral("10.0.0.1"), 0));
    QCOMPARE(backoff.failureCount(QStringLiteral("10.0.0.1"), 0), 0);
    QCOMPARE(backoff.remainingDelayMs(QStringLiteral("10.0.0.1"), 0), 0);
}

void TestAuth::backoffDoublesAndCaps()
{
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs = 1000;
    cfg.maxDelayMs  = 30000;
    lanrdp::AuthBackoff backoff(cfg);

    const QString ip = QStringLiteral("10.0.0.1");

    // 1 → 2 → 4 → 8 → 16 → 30（封顶）
    const qint64 expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    qint64 now              = 1000;
    for (qint64 want : expected) {
        const qint64 got = backoff.noteFailure(ip, now);
        QCOMPARE(got, want);
        // 未到点必须拦
        QVERIFY(!backoff.isAllowed(ip, now));
        QCOMPARE(backoff.remainingDelayMs(ip, now), want);
        // 到点必须放行（边界：恰好等于 nextAllowedAt）
        QVERIFY(backoff.isAllowed(ip, now + want));
        now += want + 1;   // 往前走一点，避免进入静默窗口
    }
    QCOMPARE(backoff.failureCount(ip, now), 7);
}

void TestAuth::backoffIsBucketedBySource()
{
    lanrdp::AuthBackoff backoff;
    const QString bad  = QStringLiteral("10.0.0.1");
    const QString good = QStringLiteral("10.0.0.2");

    backoff.noteFailure(bad, 1000);

    QVERIFY(!backoff.isAllowed(bad, 1000));
    QVERIFY2(backoff.isAllowed(good, 1000),
             "一个人猜错不能把别人也拦住（分桶的意义就在这里）");
    QCOMPARE(backoff.failureCount(good, 1000), 0);
}

void TestAuth::backoffResetsAfterQuietWindow()
{
    lanrdp::AuthBackoff::Config cfg;
    cfg.baseDelayMs    = 100;
    cfg.maxDelayMs     = 1000;
    cfg.resetWindowMs  = 5000;
    lanrdp::AuthBackoff backoff(cfg);

    const QString ip = QStringLiteral("10.0.0.1");

    backoff.noteFailure(ip, 1000);
    backoff.noteFailure(ip, 1200);
    QCOMPARE(backoff.failureCount(ip, 1200), 2);

    // 静默超过窗口 → 该来源「放下了」，再来一次应从 base 重新开始。
    const qint64 afterQuiet = 1200 + cfg.resetWindowMs + 1;
    QVERIFY(backoff.isAllowed(ip, afterQuiet));
    QCOMPARE(backoff.failureCount(ip, afterQuiet), 0);
    QCOMPARE(backoff.noteFailure(ip, afterQuiet), cfg.baseDelayMs);
}

void TestAuth::backoffSuccessClearsCounter()
{
    lanrdp::AuthBackoff backoff;
    const QString ip = QStringLiteral("10.0.0.1");

    backoff.noteFailure(ip, 1000);
    backoff.noteFailure(ip, 10000);
    QCOMPARE(backoff.failureCount(ip, 10000), 2);

    backoff.noteSuccess(ip);
    QCOMPARE(backoff.failureCount(ip, 10000), 0);
    QVERIFY(backoff.isAllowed(ip, 10000));
}

QTEST_GUILESS_MAIN(TestAuth)
#include "test_auth.moc"
