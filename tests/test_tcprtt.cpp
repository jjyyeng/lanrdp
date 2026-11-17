#include <QtTest>

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include "net/TcpRtt.h"

// 往返时间只有两种来源：自己发一帧等对方回（要改协议、要两端同版本），
// 或者问内核（Windows 的 SIO_TCP_INFO，TCP 时间戳算出来的平滑 RTT）。
// 这里选后者 —— 它不加任何线上流量，也不要求对端配合，所以「质量读数」
// 这件事不必牵动协议版本。
//
// 但这属于「本机 API 能不能用」的假设，必须被钉住：它一旦在某台机器上
// 静默返回失败，界面上只会显示「往返 —」，看起来像网络没有数据，
// 而不是像「探针坏了」。
class TestTcpRtt : public QObject
{
    Q_OBJECT

private slots:
    void loopbackReportsAnRtt();
    void unusableDescriptorIsRefused();
    void unconnectedSocketIsRefused();
};

void TestTcpRtt::loopbackReportsAnRtt()
{
    QTcpServer server;
    QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY2(client.waitForConnected(3000), qPrintable(client.errorString()));
    QVERIFY(server.waitForNewConnection(3000));
    QTcpSocket *peer = server.nextPendingConnection();
    QVERIFY(peer);

    // 至少走完一个来回：内核要靠已确认的发送才有 RTT 采样。
    QCOMPARE(client.write("x", 1), qint64(1));
    QVERIFY(client.waitForBytesWritten(2000));
    QVERIFY(peer->waitForReadyRead(2000));
    QCOMPARE(peer->write("y", 1), qint64(1));
    QVERIFY(peer->waitForBytesWritten(2000));
    QVERIFY(client.waitForReadyRead(2000));

    // 两端都要能取到（控制端读自己那侧，被控端也读自己那侧）。
    const int clientMs = lanrdp::queryTcpRttMs(client.socketDescriptor());
    const int peerMs   = lanrdp::queryTcpRttMs(peer->socketDescriptor());
    QVERIFY2(clientMs >= 0, qPrintable(QStringLiteral("控制端取不到 RTT：%1").arg(clientMs)));
    QVERIFY2(peerMs >= 0, qPrintable(QStringLiteral("被控端取不到 RTT：%1").arg(peerMs)));

    // 回环不该有几十毫秒。取到一个荒唐的大值说明字段读错了位置
    //（TCP_INFO_v0 是 ABI 结构体，错一个成员就会读出别人的数）。
    QVERIFY2(clientMs < 50, qPrintable(QStringLiteral("回环 RTT 不该是 %1 ms").arg(clientMs)));
    QVERIFY2(peerMs < 50, qPrintable(QStringLiteral("回环 RTT 不该是 %1 ms").arg(peerMs)));
}

void TestTcpRtt::unusableDescriptorIsRefused()
{
    QCOMPARE(lanrdp::queryTcpRttMs(-1), -1);
}

// 只 listen、没有对端的 socket 没有 RTT 可言 —— 必须老实返回 -1，
// 不能拿 0 冒充「一来一回 0 毫秒」。
void TestTcpRtt::unconnectedSocketIsRefused()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QVERIFY(server.socketDescriptor() >= 0);   // fd 有效，只是它还没连上谁
    QCOMPARE(lanrdp::queryTcpRttMs(server.socketDescriptor()), -1);

    QTcpSocket client;   // 从未 connectToHost：描述符本身就是 -1
    QCOMPARE(client.socketDescriptor(), qintptr(-1));
    QCOMPARE(lanrdp::queryTcpRttMs(client.socketDescriptor()), -1);
}

QTEST_MAIN(TestTcpRtt)
#include "test_tcprtt.moc"
