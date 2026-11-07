#include <QtTest/QtTest>

#include "app/WindowStatus.h"

// 窗口标题与任务栏角标的判据。
//
// 这两样东西的用途只有一个：机主在窗口最小化、或被别的窗口挡住时，
// 仍然能判断「此刻有没有人在看我的屏幕」。看错方向的代价是隐私，
// 所以判据必须钉死，而不是靠肉眼看窗口确认一遍。

class TestWindowStatus : public QObject
{
    Q_OBJECT

private slots:
    void idleTitleIsTheBaseOne();
    void listeningAloneWaits();
    void singlePeerIsNamedWithoutPort();
    void ipv6PeerLosesItsBrackets();
    void bareAddressWithoutPortIsKept();
    void emptyPeerFallsBackToGenericWording();
    void severalPeersUseTheCount();

    void badgeStaysOffUnlessSomebodyIsWatching();
    void badgeFollowsTheNumberOfConnections();

    void trayHideNeedsTheSwitch();
    void trayHideNeedsARealTray();
    void trayHideOnlyWhenMinimized();

    void closeQuitsWhenNothingIsBeingServed();
    void closeHidesWhileTheHostIsBusy();
    void closeQuitsWithoutATray();
};

void TestWindowStatus::idleTitleIsTheBaseOne()
{
    // 没在监听 ⇒ 不可能有人连着，标题不该出现任何「被控制」的字样。
    const QString title = lanrdp::hostWindowTitle(false, 0, QString());
    QCOMPARE(title, lanrdp::baseWindowTitle());
    QVERIFY2(!title.contains(QStringLiteral("控制")), qPrintable(title));
}

void TestWindowStatus::listeningAloneWaits()
{
    const QString title = lanrdp::hostWindowTitle(true, 0, QString());
    QVERIFY2(title.contains(QStringLiteral("等待连接")), qPrintable(title));
    QVERIFY2(!title.contains(QStringLiteral("正在被")), qPrintable(title));
}

void TestWindowStatus::singlePeerIsNamedWithoutPort()
{
    const QString title = lanrdp::hostWindowTitle(true, 1, QStringLiteral("192.168.11.3:52341"));
    QVERIFY2(title.contains(QStringLiteral("192.168.11.3")), qPrintable(title));
    QVERIFY2(title.contains(QStringLiteral("正在被")), qPrintable(title));
    // 端口对内网自用没有信息量，标题栏越短越好 —— 别把 ":52341" 带出去。
    QVERIFY2(!title.contains(QStringLiteral("52341")), qPrintable(title));
}

void TestWindowStatus::ipv6PeerLosesItsBrackets()
{
    QCOMPARE(lanrdp::peerAddress(QStringLiteral("[::1]:9002")), QStringLiteral("::1"));
    QCOMPARE(lanrdp::peerAddress(QStringLiteral("[fe80::1%eth0]:9002")),
             QStringLiteral("fe80::1%eth0"));

    const QString title = lanrdp::hostWindowTitle(true, 1, QStringLiteral("[::1]:9002"));
    QVERIFY2(title.contains(QStringLiteral("::1")), qPrintable(title));
    QVERIFY2(!title.contains(QStringLiteral("[")), qPrintable(title));
}

void TestWindowStatus::bareAddressWithoutPortIsKept()
{
    // 调用方只给了地址（测试 / 未来别的来源）时原样使用，不能切掉最后一段。
    QCOMPARE(lanrdp::peerAddress(QStringLiteral("192.168.11.3")),
             QStringLiteral("192.168.11.3"));
}

void TestWindowStatus::emptyPeerFallsBackToGenericWording()
{
    // 拿不到地址时退成泛称，但「正在被控制」这个事实必须留着 ——
    // 宁可不说清是谁，也不能因为缺字段就显示成「没人连」。
    const QString title = lanrdp::hostWindowTitle(true, 1, QString());
    QVERIFY2(title.contains(QStringLiteral("正在被")), qPrintable(title));
    QVERIFY2(title.contains(QStringLiteral("对方")), qPrintable(title));
}

void TestWindowStatus::severalPeersUseTheCount()
{
    const QString title = lanrdp::hostWindowTitle(true, 3, QStringLiteral("192.168.11.3:52341"));
    QVERIFY2(title.contains(QStringLiteral("3")), qPrintable(title));
    // 多路时点名一个人反而误导 —— 只报数量。
    QVERIFY2(!title.contains(QStringLiteral("192.168.11.3")), qPrintable(title));
}

void TestWindowStatus::badgeStaysOffUnlessSomebodyIsWatching()
{
    // 角标的语义是「有人正在看」，不是「我在待客」—— 监听中空着也不该有角标。
    QCOMPARE(lanrdp::hostBadgeCount(false, 0), 0);
    QCOMPARE(lanrdp::hostBadgeCount(false, 2), 0);   // 未监听时即便计数残留也不显示
    QCOMPARE(lanrdp::hostBadgeCount(true, 0), 0);
    QCOMPARE(lanrdp::hostBadgeCount(true, -1), 0);
}

void TestWindowStatus::badgeFollowsTheNumberOfConnections()
{
    QCOMPARE(lanrdp::hostBadgeCount(true, 1), 1);
    QCOMPARE(lanrdp::hostBadgeCount(true, 4), 4);
}

void TestWindowStatus::trayHideNeedsTheSwitch()
{
    // 开关关着时，最小化就只是最小化 —— 窗口得老实待在任务栏上。
    QVERIFY(!lanrdp::hideToTrayOnMinimize(false, true, true));
    QVERIFY(lanrdp::hideToTrayOnMinimize(true, true, true));
}

void TestWindowStatus::trayHideNeedsARealTray()
{
    // 这条是保命的：没有通知区域却把窗口藏起来，它就没有任何地方能叫回来，
    // 用户只能去任务管理器结束进程。
    QVERIFY(!lanrdp::hideToTrayOnMinimize(true, false, true));
}

void TestWindowStatus::trayHideOnlyWhenMinimized()
{
    // 放大、全屏、正常尺寸都不该触发 —— 只有最小化才收进托盘。
    QVERIFY(!lanrdp::hideToTrayOnMinimize(true, true, false));
}

void TestWindowStatus::closeQuitsWhenNothingIsBeingServed()
{
    // 没在监听、也没人连着：关窗口就该是退出。
    // 否则「我以为退了，其实还有一个在后台等人连」——这正是最该避免的误会。
    QVERIFY(lanrdp::closeAction(true, true, false) == lanrdp::CloseAction::Quit);
}

void TestWindowStatus::closeHidesWhileTheHostIsBusy()
{
    // 正在待客 / 正在被看：关窗只是把脸收起来，连接不能断。
    QVERIFY(lanrdp::closeAction(true, true, true) == lanrdp::CloseAction::HideToTray);
    // 开关没开时照常退出 —— 没打开托盘就不该有「藏」这个选项。
    QVERIFY(lanrdp::closeAction(false, true, true) == lanrdp::CloseAction::Quit);
}

void TestWindowStatus::closeQuitsWithoutATray()
{
    // 没有通知区域还藏起来 = 窗口凭空消失，比退出更糟。
    QVERIFY(lanrdp::closeAction(true, false, true) == lanrdp::CloseAction::Quit);
}

QTEST_GUILESS_MAIN(TestWindowStatus)
#include "test_windowstatus.moc"
