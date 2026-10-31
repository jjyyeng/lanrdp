#include <QtTest/QtTest>

#include <QImage>
#include <QVector>

#include <dxgi.h>

#include "host/ScreenCapturer.h"

// DXGI Desktop Duplication 抓屏
// 注意：若当前环境无可复制的桌面会话（锁屏 / 服务态），本测试会跳过。
class TestCapture : public QObject
{
    Q_OBJECT

private slots:
    void capturesDesktopFrame();
    void acceptsOnlyBgra8Format();

    // 多屏：枚举顺序必须稳定且与「按 index 抓屏」指向同一块屏
    void enumeratesOutputsInStableOrder();
    void indexOutOfRangeFallsBackToPrimary();
    void monitorRectMatchesEnumeratedIndex();
    void enumeratesOnlyAttachedOutputs();
};

// 格式白名单：只有 B8G8R8A8_UNORM 能走下游那条「每像素 4 字节 BGRA」的解读路径
// （`rowBytes = width * 4` + `QImage::Format_ARGB32`）。
//
// 这是纯函数，**不需要桌面会话** —— 所以它不像 capturesDesktopFrame 那样会被跳过，
// 在没有可复制桌面的机器上也能跑。真实 HDR 屏上的取样路径则无法自动化（见交付记录）。
void TestCapture::acceptsOnlyBgra8Format()
{
    QVERIFY(lanrdp::ScreenCapturer::isSupportedFormat(DXGI_FORMAT_B8G8R8A8_UNORM));

    // HDR10 常见格式：仍是 4 字节/像素，但通道位宽不同 ⇒ 会**错色**
    QVERIFY(!lanrdp::ScreenCapturer::isSupportedFormat(DXGI_FORMAT_R10G10B10A2_UNORM));
    // 每像素 8 字节 ⇒ 行距翻倍，只拷 width*4 会**错行**
    QVERIFY(!lanrdp::ScreenCapturer::isSupportedFormat(DXGI_FORMAT_R16G16B16A16_FLOAT));

    QVERIFY(!lanrdp::ScreenCapturer::isSupportedFormat(DXGI_FORMAT_UNKNOWN));
    QVERIFY(!lanrdp::ScreenCapturer::isSupportedFormat(0xFFFFFFFFu));
}

void TestCapture::capturesDesktopFrame()
{
    lanrdp::ScreenCapturer capturer;
    QString                err;
    QByteArray             skipMsg;

    if (!capturer.init(&err)) {
        skipMsg = QStringLiteral("抓屏不可用，跳过：%1").arg(err).toUtf8();
        QSKIP(skipMsg.constData());
    }

    QVERIFY(capturer.isReady());
    QVERIFY(capturer.width() > 0);
    QVERIFY(capturer.height() > 0);

    QImage                 frame;
    QString                lastErr;
    lanrdp::CaptureStatus  status = lanrdp::CaptureStatus::NoNewFrame;

    // 桌面无变化时 AcquireNextFrame 会超时，重试若干次
    for (int attempt = 0; attempt < 30; ++attempt) {
        status = capturer.capture(&frame, 200, &lastErr);
        if (status != lanrdp::CaptureStatus::NoNewFrame)
            break;
    }

    QVERIFY2(status != lanrdp::CaptureStatus::Failed, qPrintable(lastErr));
    QCOMPARE(status, lanrdp::CaptureStatus::Ok);
    QVERIFY(!frame.isNull());
    QCOMPARE(frame.size(), QSize(capturer.width(), capturer.height()));
}

// 枚举顺序决定「屏 1 / 屏 2」在界面上的含义，也决定 `SCREEN_SELECT` 的 index 指向哪块屏。
// 两次调用必须给出同一串结果 —— 否则「控制端选屏 2、被控端抓到屏 1」这种错位会随机出现。
void TestCapture::enumeratesOutputsInStableOrder()
{
    const QVector<lanrdp::MonitorInfo> first  = lanrdp::ScreenCapturer::enumerateOutputs();
    const QVector<lanrdp::MonitorInfo> second = lanrdp::ScreenCapturer::enumerateOutputs();

    if (first.isEmpty())
        QSKIP("枚举不到任何已附着的显示输出（无桌面会话？），跳过");

    QCOMPARE(second.size(), first.size());
    for (int i = 0; i < first.size(); ++i) {
        QCOMPARE(second.at(i).index, i);
        QCOMPARE(second.at(i).rect, first.at(i).rect);
        QCOMPARE(second.at(i).isPrimary, first.at(i).isPrimary);
    }

    // 主屏恰好一个（多屏时 Win32 的定义就是如此）
    int primaries = 0;
    for (const auto &m : first)
        primaries += m.isPrimary ? 1 : 0;
    QCOMPARE(primaries, 1);
}

// 屏拔掉了 / 枚举顺序变了 ⇒ 控制端手上那个 index 可能越界。
// 此时必须回落到主屏并且**照常能抓**，而不是 init 失败（失败等于整条链路黑屏，
// 而用户只是切了个屏）。
void TestCapture::indexOutOfRangeFallsBackToPrimary()
{
    const QVector<lanrdp::MonitorInfo> monitors = lanrdp::ScreenCapturer::enumerateOutputs();
    if (monitors.isEmpty())
        QSKIP("枚举不到任何已附着的显示输出（无桌面会话？），跳过");

    QRect primaryRect;
    for (const auto &m : monitors) {
        if (m.isPrimary)
            primaryRect = m.rect;
    }
    QVERIFY(!primaryRect.isEmpty());

    lanrdp::ScreenCapturer capturer;
    QString                err;
    if (!capturer.init(&err, nullptr, /*preferredOutputIndex=*/9999)) {
        const QByteArray skipMsg = QStringLiteral("抓屏不可用，跳过：%1").arg(err).toUtf8();
        QSKIP(skipMsg.constData());
    }

    QVERIFY(capturer.isReady());
    QCOMPARE(capturer.monitorRect(), primaryRect);
    // 回落之后报出来的 index 必须是**真实的**那一个（界面按它打勾，
    // 继续报 9999 会让控制端菜单勾在一块不存在的屏上）
    QVERIFY(capturer.outputIndex() >= 0);
    QVERIFY(capturer.outputIndex() < monitors.size());
}

// 「枚举列表的第 i 项」与「按 index = i 初始化抓到的东西」必须是同一块屏。
// 这条是本项最容易写错的地方：枚举和抓屏各写一遍遍历，顺序稍有不同就错位。
void TestCapture::monitorRectMatchesEnumeratedIndex()
{
    const QVector<lanrdp::MonitorInfo> monitors = lanrdp::ScreenCapturer::enumerateOutputs();
    if (monitors.isEmpty())
        QSKIP("枚举不到任何已附着的显示输出（无桌面会话？），跳过");

    for (int i = 0; i < monitors.size(); ++i) {
        lanrdp::ScreenCapturer capturer;
        QString                err;
        if (!capturer.init(&err, nullptr, i)) {
            const QByteArray skipMsg = QStringLiteral("抓屏不可用，跳过：%1").arg(err).toUtf8();
            QSKIP(skipMsg.constData());
        }
        QCOMPARE(capturer.outputIndex(), i);
        QCOMPARE(capturer.monitorRect(), monitors.at(i).rect);
        QCOMPARE(capturer.width(), monitors.at(i).rect.width());
        QCOMPARE(capturer.height(), monitors.at(i).rect.height());
    }
}

// 未附着桌面的输出（休眠 / 被禁用 / 虚拟显示器的空输出）不参与枚举，
// 否则界面上会多出一项点了黑屏的「屏 3」。
void TestCapture::enumeratesOnlyAttachedOutputs()
{
    const QVector<lanrdp::MonitorInfo> monitors = lanrdp::ScreenCapturer::enumerateOutputs();
    if (monitors.isEmpty())
        QSKIP("枚举不到任何已附着的显示输出（无桌面会话？），跳过");

    for (const auto &m : monitors) {
        QVERIFY2(!m.rect.isEmpty(),
                 qPrintable(QStringLiteral("第 %1 项矩形为空，说明混进了未附着的输出").arg(m.index)));
        QVERIFY(m.rect.width() > 0 && m.rect.height() > 0);
    }
}

QTEST_GUILESS_MAIN(TestCapture)
#include "test_capture.moc"
