#include <QtTest/QtTest>

#include <QTcpSocket>

#include "net/Auth.h"
#include "net/HostServer.h"
#include "net/Session.h"

// 双角色入口端到端骨架（HostServer + AuthClient 走真实 loopback）
class TestHandshake : public QObject
{
    Q_OBJECT

private slots:
    void acceptsCorrectPasswordEndToEnd();
    void rejectsWrongPasswordEndToEnd();
};

void TestHandshake::acceptsCorrectPasswordEndToEnd()
{
    lanrdp::HostServer host(QStringLiteral("s3cret"));
    QVERIFY(host.listen(0));  // 0 = 系统分配临时端口
    QVERIFY(host.port() != 0);

    bool serverOk = false;
    QObject::connect(&host, &lanrdp::HostServer::clientAuthenticated,
                     [&serverOk]() { serverOk = true; });
    bool serverDenied = false;
    QObject::connect(&host, &lanrdp::HostServer::clientRejected,
                     [&serverDenied]() { serverDenied = true; });

    auto *socket = new QTcpSocket();  // Session 接管所有权
    socket->connectToHost(QHostAddress::LocalHost, host.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session clientSession(socket);
    lanrdp::AuthClient auth(&clientSession, QStringLiteral("s3cret"));
    bool clientOk = false;
    QObject::connect(&auth, &lanrdp::AuthClient::succeeded, [&clientOk]() { clientOk = true; });
    auth.start();

    QTRY_VERIFY(serverOk);
    QTRY_VERIFY(clientOk);
    QVERIFY(!serverDenied);
    QCOMPARE(host.sessions().size(), 1);
}

void TestHandshake::rejectsWrongPasswordEndToEnd()
{
    lanrdp::HostServer host(QStringLiteral("s3cret"));
    QVERIFY(host.listen(0));

    bool serverOk = false;
    QObject::connect(&host, &lanrdp::HostServer::clientAuthenticated,
                     [&serverOk]() { serverOk = true; });
    bool serverDenied = false;
    QObject::connect(&host, &lanrdp::HostServer::clientRejected,
                     [&serverDenied]() { serverDenied = true; });

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, host.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session clientSession(socket);
    lanrdp::AuthClient auth(&clientSession, QStringLiteral("wrong-password"));
    bool clientFailed = false;
    QObject::connect(&auth, &lanrdp::AuthClient::failed, [&clientFailed]() { clientFailed = true; });
    bool clientOk = false;
    QObject::connect(&auth, &lanrdp::AuthClient::succeeded, [&clientOk]() { clientOk = true; });
    auth.start();

    QTRY_VERIFY(serverDenied);
    QTRY_VERIFY(clientFailed);
    QVERIFY(!serverOk);
    QVERIFY(!clientOk);
    QCOMPARE(host.sessions().size(), 0);
}

QTEST_GUILESS_MAIN(TestHandshake)
#include "test_handshake.moc"
