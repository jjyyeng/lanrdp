#include <QtTest/QtTest>

#include <QImage>

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

QTEST_GUILESS_MAIN(TestCapture)
#include "test_capture.moc"
