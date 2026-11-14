#include <QtTest>

#include <QAbstractSocket>

#include "common/ConnectionHints.h"

// 控制端「连不上」的下一步建议。
//
// 为什么值得单独钉：socket->errorString() 是英文，而且只描述现象
// （"Connection refused" / "Network is unreachable"）。用户看到这句话
// 并不知道该去查防火墙、改端口还是确认对方点没点【开始监听】。
class TestConnectionHints : public QObject
{
    Q_OBJECT

private slots:
    void refusedPointsAtTheOtherSide();
    void timeoutMentionsFirewallAndSubnet();
    void hostNotFoundMentionsAddress();
    void remoteClosedMentionsStopListening();
    void unknownErrorYieldsNoHint();
};

// 对端没监听（RST）：最可能是对方没点开始监听，或端口号对不上。
// 这一种**不要**提防火墙 —— 包到了、被拒了，说明网络是通的。
void TestConnectionHints::refusedPointsAtTheOtherSide()
{
    const QString hint = lanrdp::connectionHint(QAbstractSocket::ConnectionRefusedError);
    QVERIFY2(hint.contains(QStringLiteral("监听")), qPrintable(hint));
    QVERIFY2(hint.contains(QStringLiteral("端口")), qPrintable(hint));
    QVERIFY2(!hint.contains(QStringLiteral("防火墙")),
             qPrintable(QStringLiteral("被拒说明包到了，不该往防火墙引：") + hint));
}

// 超时（包被丢）：防火墙 / 网段不对。这一种才该提放行。
void TestConnectionHints::timeoutMentionsFirewallAndSubnet()
{
    const QString hint = lanrdp::connectionHint(QAbstractSocket::SocketTimeoutError);
    QVERIFY2(hint.contains(QStringLiteral("防火墙")), qPrintable(hint));
    QVERIFY2(hint.contains(QStringLiteral("网段")), qPrintable(hint));
}

void TestConnectionHints::hostNotFoundMentionsAddress()
{
    const QString hint = lanrdp::connectionHint(QAbstractSocket::HostNotFoundError);
    QVERIFY2(hint.contains(QStringLiteral("地址")), qPrintable(hint));
}

void TestConnectionHints::remoteClosedMentionsStopListening()
{
    const QString hint = lanrdp::connectionHint(QAbstractSocket::RemoteHostClosedError);
    QVERIFY2(hint.contains(QStringLiteral("停止")), qPrintable(hint));
}

// 未知错误不硬编一句可能指错方向的话：只说现象，别猜原因。
void TestConnectionHints::unknownErrorYieldsNoHint()
{
    QVERIFY(lanrdp::connectionHint(QAbstractSocket::UnknownSocketError).isEmpty());
}

QTEST_MAIN(TestConnectionHints)
#include "test_connectionhints.moc"
