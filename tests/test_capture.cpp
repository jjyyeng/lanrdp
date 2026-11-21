#include <QtTest/QtTest>

#include <QImage>
#include <QVector>

#include <dxgi.h>

#include "common/ColorConvert.h"
#include "host/ScreenCapturer.h"

// DXGI Desktop Duplication 抓屏
// 注意：若当前环境无可复制的桌面会话（锁屏 / 服务态），本测试会跳过。
class TestCapture : public QObject
{
    Q_OBJECT

private slots:
    void capturesDesktopFrame();
    // NV12 直出（H.264 正路）：结构、取值范围、与老路径同源、以及「走显卡还是 CPU」
    void capturesNv12FrameDirectly();
    // 设置里关掉显卡后，抓屏仍必须出帧（只是走 CPU 直读），并且能说清是「手动关的」
    void gpuConversionCanBeDisabled();
    void acceptsOnlyBgra8Format();

    // 多屏：枚举顺序必须稳定且与「按 index 抓屏」指向同一块屏
    void enumeratesOutputsInStableOrder();
    void indexOutOfRangeFallsBackToPrimary();
    void monitorRectMatchesEnumeratedIndex();
    void enumeratesOnlyAttachedOutputs();
};

// captureNv12 是 4K 下的正路（少一次 33 MB 往返）。像素级一致性由
// test_nv12converter 断言（同一张纹理同时走显卡与 CPU 比偏差）；这里验的是
// **真实桌面链路**上的三件事：紧凑布局、BT.601 有限范围、与老路径同源。
// 第二条专抓「显卡驱动忽略了输出色彩空间（回落到 0-255）」，那种错零报错、只偏色。
void TestCapture::capturesNv12FrameDirectly()
{
    lanrdp::ScreenCapturer capturer;
    QString                err;
    if (!capturer.init(&err)) {
        QByteArray skipMsg = QStringLiteral("抓屏不可用，跳过：%1").arg(err).toUtf8();
        QSKIP(skipMsg.constData());
    }

    const int width  = capturer.width();
    const int height = capturer.height();
    QVERIFY(width > 0 && height > 0);
    QVERIFY2((width % 2) == 0 && (height % 2) == 0,
             "NV12 要求偶数宽高；桌面复制给的是物理像素，这条不该失败");

    // 两条路各抓一帧再比整帧均值。
    // ⚠️ 要重试：显示器刚从休眠里醒来时，DXGI 前几帧是**全黑**的（画面还没真正合成），
    // 这时候比出来的差值是环境噪声，不是显卡路径的问题。最多试 10 次，任一对得上即通过。
    QByteArray nv12;
    QString    lastErr;
    double     meanNv12  = -1.0;
    double     meanRef   = -1.0;
    int        minY      = 0;
    int        maxY      = 0;
    bool       matched   = false;

    for (int attempt = 0; attempt < 10 && !matched; ++attempt) {
        QString               errText;
        lanrdp::CaptureStatus status = lanrdp::CaptureStatus::NoNewFrame;
        for (int retry = 0; retry < 30; ++retry) {
            status = capturer.captureNv12(&nv12, 200, &errText);
            if (status != lanrdp::CaptureStatus::NoNewFrame)
                break;
        }
        QVERIFY2(status != lanrdp::CaptureStatus::Failed, qPrintable(errText));
        QCOMPARE(status, lanrdp::CaptureStatus::Ok);

        // 紧凑 NV12：Y 平面 + 交织 UV（w/2 * h/2 对）
        QCOMPARE(nv12.size(), width * height + (width / 2) * (height / 2) * 2);

        const auto *y     = reinterpret_cast<const uchar *>(nv12.constData());
        const int   ySize = width * height;
        minY = 255;
        maxY = 0;
        qint64 sumY = 0;
        for (int i = 0; i < ySize; ++i) {
            minY = qMin(minY, static_cast<int>(y[i]));
            maxY = qMax(maxY, static_cast<int>(y[i]));
            sumY += y[i];
        }
        // BT.601 有限范围：Y 必须落在 16..235。显卡驱动若忽略我们的输出色彩空间，
        // 会回落成全范围 0-255 ⇒ 这里当场失败（而不是等用户看到偏色再来查）。
        QVERIFY2(minY >= 16 && maxY <= 235,
                 qPrintable(QStringLiteral("Y 平面越界（%1..%2）：输出不是有限范围，解码端会偏色")
                                .arg(minY)
                                .arg(maxY)));

        // 参考帧：同一块屏的老路径（QImage -> NV12）
        QImage                refImage;
        QString               refError;
        lanrdp::CaptureStatus refStatus = lanrdp::CaptureStatus::NoNewFrame;
        for (int retry = 0; retry < 30; ++retry) {
            refStatus = capturer.capture(&refImage, 200, &refError);
            if (refStatus != lanrdp::CaptureStatus::NoNewFrame)
                break;
        }
        if (refStatus != lanrdp::CaptureStatus::Ok || refImage.isNull()
            || refImage.size() != QSize(width, height))
            continue;

        const QByteArray reference =
            lanrdp::bgraToNv12(refImage.constBits(), static_cast<int>(refImage.bytesPerLine()),
                               width, height);
        if (reference.isEmpty())
            continue;

        const auto *ry     = reinterpret_cast<const uchar *>(reference.constData());
        int         refMin = 255;
        int         refMax = 0;
        qint64      refSum = 0;
        for (int i = 0; i < ySize; ++i) {
            refMin = qMin(refMin, static_cast<int>(ry[i]));
            refMax = qMax(refMax, static_cast<int>(ry[i]));
            refSum += ry[i];
        }

        meanNv12 = static_cast<double>(sumY) / ySize;
        meanRef  = static_cast<double>(refSum) / ySize;

        // 只比整帧均值：两次抓帧之间画面可能微动（拖窗 / 时间角标），逐像素比会抖；
        // 均值差 >12 就说明两条路拿到的不是同一幅内容。
        if (qAbs(meanNv12 - meanRef) <= 12.0) {
            // 老路径有明暗变化时，快速路径不能给一块纯色（「Blt 没生效」的典型表现）
            if (refMax - refMin > 8) {
                QVERIFY2(maxY > minY,
                         qPrintable(QStringLiteral("参考帧有明暗差（%1..%2），NV12 却整帧同值 %3")
                                        .arg(refMin)
                                        .arg(refMax)
                                        .arg(minY)));
            }
            matched = true;
        }
    }

    QVERIFY2(matched,
             qPrintable(QStringLiteral("两条路画面均值始终对不上（NV12 %1 / QImage %2）："
                                       "显卡路径可能没真的把桌面画进去")
                            .arg(meanNv12, 0, 'f', 1)
                            .arg(meanRef, 0, 'f', 1)));

    // 「用的是显卡还是 CPU 直读」必须可查 —— 驱动可能静默降级，那是用户唯一的线索
    qInfo("NV12 路径：%s", qPrintable(capturer.nv12PathInfo()));
    QVERIFY2(!capturer.nv12PathInfo().isEmpty(), "降级/启用原因必须可读，否则用户无从判断");

    // 连抓两帧不崩、不泄漏帧（ReleaseFrame 漏一次，下一次 AcquireNextFrame 就会失败）
    QByteArray second;
    QString    secondErr;
    const lanrdp::CaptureStatus secondStatus = capturer.captureNv12(&second, 200, &secondErr);
    QVERIFY2(secondStatus == lanrdp::CaptureStatus::Ok
                 || secondStatus == lanrdp::CaptureStatus::NoNewFrame,
             qPrintable(secondErr));
}

void TestCapture::gpuConversionCanBeDisabled()
{
    lanrdp::ScreenCapturer capturer;
    QString                err;
    if (!capturer.init(&err)) {
        QByteArray skipMsg = QStringLiteral("抓屏不可用，跳过：%1").arg(err).toUtf8();
        QSKIP(skipMsg.constData());
    }

    // 设置里关掉显卡：**帧不能丢、输出格式不能变**，只是换 CPU 直读。
    // 这条路径是用户的排障出路 —— 它要是也坏了，那这个开关就只是摆设。
    capturer.setGpuConversionEnabled(false);

    auto grabNv12 = [&capturer](QByteArray *out, QString *error) {
        lanrdp::CaptureStatus status = lanrdp::CaptureStatus::NoNewFrame;
        for (int retry = 0; retry < 30; ++retry) {
            status = capturer.captureNv12(out, 200, error);
            if (status != lanrdp::CaptureStatus::NoNewFrame)
                break;
        }
        return status;
    };

    QByteArray nv12;
    QString    frameError;
    QCOMPARE(grabNv12(&nv12, &frameError), lanrdp::CaptureStatus::Ok);
    QVERIFY2(!nv12.isEmpty(), qPrintable(frameError));
    QCOMPARE(capturer.width() * capturer.height() + (capturer.width() / 2) * (capturer.height() / 2) * 2,
             nv12.size());

    const QString disabledNote = capturer.nv12PathInfo();
    QVERIFY2(disabledNote.contains(QStringLiteral("手动")),
             qPrintable(QStringLiteral("关掉之后没说清是谁关的：%1").arg(disabledNote)));

    // 重新打开：路径结论必须跟着变。停在旧结论上，用户会以为开关点了没反应。
    capturer.setGpuConversionEnabled(true);
    QByteArray again;
    QString    againError;
    QCOMPARE(grabNv12(&again, &againError), lanrdp::CaptureStatus::Ok);
    QVERIFY2(!capturer.nv12PathInfo().contains(QStringLiteral("手动")),
             qPrintable(QStringLiteral("重新打开后仍报手动关闭：%1").arg(capturer.nv12PathInfo())));
}

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
