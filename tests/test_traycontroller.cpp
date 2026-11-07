#include <QtTest/QtTest>

#include "app/TrayController.h"

#include <QColor>
#include <QImage>

// 托盘常驻的判据。
//
// 两件事必须钉死：① 悬停提示与窗口标题同口径（两处说法不一致，人会以为看的
// 是两件事）；② 图标上的状态点真的画出来了 —— 这条只能量像素，光看「函数被
// 调用了」测不出画布是不是空的。
//
// 通知区域本身在 qoffscreen 下不存在，所以「真的显示出来」这一面测不到；
// 能测的是「不可用时不许崩、状态照样记得住」，这正是上层判断「关窗口会不会
// 变成看不见的进程」所依赖的那一半。

namespace {

// 数一数红色像素。与任务栏角标同一套判定：色相明显偏红、且明显不是灰。
int countRedPixels(const QPixmap &pm)
{
    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    int            n   = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.red() > 150 && c.green() < 120 && c.blue() < 130 && c.red() - c.green() > 60)
                ++n;
        }
    }
    return n;
}

} // namespace

class TestTrayController : public QObject
{
    Q_OBJECT

private slots:
    void toolTipWaitsWhenNobodyConnected();
    void toolTipNamesSinglePeer();
    void toolTipCountsSeveralPeers();
    void toolTipDropsThePort();

    void sharedIconCarriesTheRedDot();
    void listeningIconHasNoDot();
    void iconHonoursTheRequestedPixelSize();

    void statusIsRecordedEvenWithoutATray();
    void visibilityFollowsThePlatformNotTheRequest();
    void repeatedRequestsKeepTryingWithoutATray();
    void hidingClearsTheVisibilityClaim();
    void notifyIsHarmlessWithoutATray();
};

void TestTrayController::toolTipWaitsWhenNobodyConnected()
{
    const QString tip = lanrdp::trayToolTip(lanrdp::TrayStatus::Listening, 0, QString());
    QVERIFY2(tip.contains(QStringLiteral("等待连接")), qPrintable(tip));
    QVERIFY2(!tip.contains(QStringLiteral("正在被")), qPrintable(tip));
}

void TestTrayController::toolTipNamesSinglePeer()
{
    const QString tip = lanrdp::trayToolTip(lanrdp::TrayStatus::Shared, 1,
                                            QStringLiteral("192.168.11.3:9002"));
    QVERIFY2(tip.contains(QStringLiteral("正在被")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("192.168.11.3")), qPrintable(tip));
}

void TestTrayController::toolTipCountsSeveralPeers()
{
    const QString tip = lanrdp::trayToolTip(lanrdp::TrayStatus::Shared, 3, QString());
    // 多个连接时不点名，改成报数 —— 逐个列出来在悬停提示里读不过来。
    QVERIFY2(tip.contains(QStringLiteral("3")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("个连接")), qPrintable(tip));
}

void TestTrayController::toolTipDropsThePort()
{
    const QString tip = lanrdp::trayToolTip(lanrdp::TrayStatus::Shared, 1,
                                            QStringLiteral("192.168.11.3:9002"));
    // 端口是本地实现细节，提示里只留地址。
    QVERIFY2(!tip.contains(QStringLiteral("9002")), qPrintable(tip));
}

void TestTrayController::sharedIconCarriesTheRedDot()
{
    const QPixmap pm = lanrdp::trayIconPixmap(lanrdp::TrayStatus::Shared, 32);
    QVERIFY(!pm.isNull());
    QVERIFY2(countRedPixels(pm) > 20, qPrintable(QString::number(countRedPixels(pm))));
}

void TestTrayController::listeningIconHasNoDot()
{
    // 「等着」不能长得像「正在被看」—— 少一个红点，正是这两档的全部区别。
    const QPixmap pm = lanrdp::trayIconPixmap(lanrdp::TrayStatus::Listening, 32);
    QVERIFY(!pm.isNull());
    QCOMPARE(countRedPixels(pm), 0);
}

void TestTrayController::iconHonoursTheRequestedPixelSize()
{
    // QIcon 按 DPI 挑档，所以每个档都得按自己的边长重画，不能拿 16 拉伸。
    for (int px : {16, 20, 24, 32}) {
        const QPixmap pm = lanrdp::trayIconPixmap(lanrdp::TrayStatus::Shared, px);
        QCOMPARE(pm.width(), px);
        QCOMPARE(pm.height(), px);
    }
}

void TestTrayController::statusIsRecordedEvenWithoutATray()
{
    lanrdp::TrayController tray;
    QVERIFY2(!lanrdp::TrayController::available(),
             "qoffscreen 下不该有通知区域，用例前提不成立");

    tray.setStatus(lanrdp::TrayStatus::Shared, 2, QStringLiteral("192.168.11.3:9002"));
    QVERIFY(tray.status() == lanrdp::TrayStatus::Shared);
    QCOMPARE(tray.peerCount(), 2);

    tray.setStatus(lanrdp::TrayStatus::Listening, -1, QString());
    QVERIFY(tray.status() == lanrdp::TrayStatus::Listening);
    QCOMPARE(tray.peerCount(), 0);   // 负数按 0 收，别把脏值带进界面
}

void TestTrayController::visibilityFollowsThePlatformNotTheRequest()
{
    lanrdp::TrayController tray;
    tray.setStatus(lanrdp::TrayStatus::Listening);

    tray.setVisible(true);
    // 请求归请求：没有通知区域时，它永远不会真的出现。
    QVERIFY(!tray.isVisible());

    tray.setVisible(false);
    QVERIFY(!tray.isVisible());
}

void TestTrayController::repeatedRequestsKeepTryingWithoutATray()
{
    lanrdp::TrayController tray;
    QVERIFY2(!lanrdp::TrayController::available(),
             "qoffscreen 下不该有通知区域，用例前提不成立");

    // 反复请求都必须安全地落空。钉的是「不许把第一次的失败记成状态」——
    // 一旦「请求过就跳过」，通知区域晚一步就绪时图标就再没有补上的机会。
    for (int i = 0; i < 3; ++i) {
        tray.setVisible(true);
        QVERIFY(!tray.isVisible());
    }
}

void TestTrayController::hidingClearsTheVisibilityClaim()
{
    lanrdp::TrayController tray;
    tray.setVisible(true);
    tray.setVisible(false);

    // 藏起来之后必须立刻不再声称「看得见」：上层拿这个值判断能不能把窗口收走，
    // 残留一次 true 就等于放行一次无路可回的隐藏。
    QVERIFY(!tray.isVisible());
}

void TestTrayController::notifyIsHarmlessWithoutATray()
{
    lanrdp::TrayController tray;
    tray.setVisible(true);
    tray.notify(QStringLiteral("标题"), QStringLiteral("正文"));
    QVERIFY(!tray.isVisible());
}

QTEST_MAIN(TestTrayController)
#include "test_traycontroller.moc"
