#include <QtTest/QtTest>

#include <QByteArray>
#include <QImage>
#include <QVector>

#include "host/ColorConvert.h"

// BGRA -> NV12 色彩转换
// 采用 BT.601 有限范围（video range）：Y 落在 16..235，Cb/Cr 落在 16..240。
class TestColorConvert : public QObject
{
    Q_OBJECT

private slots:
    void blackAndWhiteHitLegalRangeEnds();
    void primariesMatchBt601();
    void producesNv12LayoutWithAveragedChroma();
    void honoursSourceStride();
    void handlesOddDimensions();
    void rejectsInvalidInput();
    void nv12RoundTripsBackToRgb();

    // 转换 + 缩放（把「写全尺寸 ARGB32」这一步省掉）
    void scaledConversionFitsRequestedBox();
    void scaledConversionKeepsAspectRatio();
    void scaledConversionNeverUpscales();
    void scaledConversionSamplesSourceRegion();
    void scaledConversionFallsBackOnDegenerateBox();
    // 收益守卫的阈值定档（1/9）：面积比 > 1/9 回退全尺寸，恰好 1/9 仍缩
    void scaledConversionSkipsWhenNotWorthIt();
    void scaledConversionThresholdBoundaryIsInclusive();
};

namespace {

// 生成一张 width x height 的 BGRA 图，stride 可控（默认紧凑）
QByteArray makeBgra(int width, int height, const QVector<QRgb> &pixels, int stride = 0)
{
    const int rowBytes = stride > 0 ? stride : width * 4;
    QByteArray data(rowBytes * height, '\0');
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const QRgb c = pixels.at(y * width + x);
            char *p = data.data() + y * rowBytes + x * 4;
            p[0] = static_cast<char>(qBlue(c));
            p[1] = static_cast<char>(qGreen(c));
            p[2] = static_cast<char>(qRed(c));
            p[3] = static_cast<char>(255);
        }
    }
    return data;
}

QVector<QRgb> solid(int width, int height, QRgb color)
{
    QVector<QRgb> pixels(width * height);
    pixels.fill(color);
    return pixels;
}

int yAt(const QByteArray &nv12, int width, int x, int y)
{
    return static_cast<quint8>(nv12.at(y * width + x));
}

// NV12 的 UV 平面在 Y 平面之后，交织为 U,V,U,V...
int uvAt(const QByteArray &nv12, int width, int height, int cx, int cy)
{
    const int chromaWidth = (width + 1) / 2;
    const int base        = width * height + (cy * chromaWidth + cx) * 2;
    return static_cast<quint8>(nv12.at(base));
}

int uvAtV(const QByteArray &nv12, int width, int height, int cx, int cy)
{
    const int chromaWidth = (width + 1) / 2;
    const int base        = width * height + (cy * chromaWidth + cx) * 2 + 1;
    return static_cast<quint8>(nv12.at(base));
}

} // namespace

void TestColorConvert::blackAndWhiteHitLegalRangeEnds()
{
    {
        const QByteArray src = makeBgra(4, 4, solid(4, 4, qRgb(255, 255, 255)));
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(src.constData()), 4 * 4, 4, 4);
        QCOMPARE(nv12.size(), 4 * 4 + 2 * 2 * 2);
        QCOMPARE(yAt(nv12, 4, 0, 0), 235);              // 白 -> Y 上限
        QCOMPARE(uvAt(nv12, 4, 4, 0, 0), 128);          // 无色度
        QCOMPARE(uvAtV(nv12, 4, 4, 0, 0), 128);
    }
    {
        const QByteArray src = makeBgra(4, 4, solid(4, 4, qRgb(0, 0, 0)));
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(src.constData()), 4 * 4, 4, 4);
        QCOMPARE(yAt(nv12, 4, 0, 0), 16);               // 黑 -> Y 下限
        QCOMPARE(uvAt(nv12, 4, 4, 0, 0), 128);
        QCOMPARE(uvAtV(nv12, 4, 4, 0, 0), 128);
    }
}

void TestColorConvert::primariesMatchBt601()
{
    struct Case { QRgb color; int y; int u; int v; const char *name; };
    const Case cases[] = {
        {qRgb(255, 0, 0),   82,  90, 240, "red"},
        {qRgb(0, 255, 0),  144,  54,  34, "green"},
        {qRgb(0, 0, 255),   41, 240, 110, "blue"},
    };

    for (const Case &c : cases) {
        const QByteArray src  = makeBgra(2, 2, solid(2, 2, c.color));
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(src.constData()), 2 * 4, 2, 2);

        // 整数定点与浮点 BT.601 之间允许 ±2 的舍入差
        QVERIFY2(qAbs(yAt(nv12, 2, 0, 0) - c.y) <= 2,
                 qPrintable(QStringLiteral("%1 Y: got %2 want %3")
                                .arg(QLatin1String(c.name))
                                .arg(yAt(nv12, 2, 0, 0))
                                .arg(c.y)));
        QVERIFY2(qAbs(uvAt(nv12, 2, 2, 0, 0) - c.u) <= 2,
                 qPrintable(QStringLiteral("%1 U: got %2 want %3")
                                .arg(QLatin1String(c.name))
                                .arg(uvAt(nv12, 2, 2, 0, 0))
                                .arg(c.u)));
        QVERIFY2(qAbs(uvAtV(nv12, 2, 2, 0, 0) - c.v) <= 2,
                 qPrintable(QStringLiteral("%1 V: got %2 want %3")
                                .arg(QLatin1String(c.name))
                                .arg(uvAtV(nv12, 2, 2, 0, 0))
                                .arg(c.v)));
    }
}

void TestColorConvert::producesNv12LayoutWithAveragedChroma()
{
    // 左半黑右半白：色度应取四像素平均后的中性灰，Y 则左右不同
    QVector<QRgb> pixels(4 * 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            pixels[y * 4 + x] = (x < 2) ? qRgb(0, 0, 0) : qRgb(255, 255, 255);

    const QByteArray src  = makeBgra(4, 4, pixels);
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(src.constData()), 4 * 4, 4, 4);

    // 尺寸：Y 平面 + (w/2 * h/2) 个交织 UV 对
    QCOMPARE(nv12.size(), 4 * 4 + 2 * 2 * 2);

    QCOMPARE(yAt(nv12, 4, 0, 0), 16);    // 左：黑
    QCOMPARE(yAt(nv12, 4, 3, 0), 235);   // 右：白

    // 第一个色度块覆盖左侧 2x2（全黑）-> 中性
    QCOMPARE(uvAt(nv12, 4, 4, 0, 0), 128);
    QCOMPARE(uvAtV(nv12, 4, 4, 0, 0), 128);
    // 第二个色度块覆盖右侧 2x2（全白）-> 中性
    QCOMPARE(uvAt(nv12, 4, 4, 1, 0), 128);
    QCOMPARE(uvAtV(nv12, 4, 4, 1, 0), 128);
}

void TestColorConvert::honoursSourceStride()
{
    // 同样内容，但源每行后面留 64 字节padding；结果必须与紧凑时一致
    QVector<QRgb> pixels(8 * 4);
    for (int i = 0; i < pixels.size(); ++i)
        pixels[i] = qRgb((i * 7) % 256, (i * 13) % 256, (i * 29) % 256);

    const QByteArray tight  = makeBgra(8, 4, pixels, 8 * 4);
    const QByteArray padded = makeBgra(8, 4, pixels, 8 * 4 + 64);

    const QByteArray a = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(tight.constData()), 8 * 4, 8, 4);
    const QByteArray b = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(padded.constData()), 8 * 4 + 64, 8, 4);

    QVERIFY(!a.isEmpty());
    QCOMPARE(a, b);
}

void TestColorConvert::handlesOddDimensions()
{
    // 奇数宽高不能越界；色度平面向上取整
    const QByteArray src  = makeBgra(5, 3, solid(5, 3, qRgb(10, 200, 30)));
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(src.constData()), 5 * 4, 5, 3);

    QCOMPARE(nv12.size(), 5 * 3 + 3 * 2 * 2);   // chroma 3x2
}

void TestColorConvert::rejectsInvalidInput()
{
    const QByteArray src = makeBgra(4, 4, solid(4, 4, qRgb(1, 2, 3)));
    const auto         p = reinterpret_cast<const uchar *>(src.constData());

    QVERIFY(lanrdp::bgraToNv12(nullptr, 16, 4, 4).isEmpty());
    QVERIFY(lanrdp::bgraToNv12(p, 16, 0, 4).isEmpty());
    QVERIFY(lanrdp::bgraToNv12(p, 16, 4, 0).isEmpty());
    QVERIFY(lanrdp::bgraToNv12(p, 8, 4, 4).isEmpty());   // stride 小于一行
}

void TestColorConvert::nv12RoundTripsBackToRgb()
{
    // 有限范围的 YUV 本身有损，纯色往返后允许 ±4 的通道误差
    const QRgb colors[] = {qRgb(255, 255, 255), qRgb(0, 0, 0), qRgb(255, 0, 0),
                           qRgb(0, 255, 0),     qRgb(0, 0, 255), qRgb(64, 128, 192)};

    for (const QRgb color : colors) {
        const int      side = 8;
        const QByteArray src = makeBgra(side, side, solid(side, side, color));
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(src.constData()), side * 4, side, side);
        QVERIFY(!nv12.isEmpty());

        const QImage image = lanrdp::nv12ToArgb32(
            reinterpret_cast<const uchar *>(nv12.constData()), side, side, side);
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(side, side));

        const QRgb got = image.pixel(4, 4);
        const QString detail = QStringLiteral("color #%1 -> #%2")
                                   .arg(color, 8, 16, QLatin1Char('0'))
                                   .arg(got, 8, 16, QLatin1Char('0'));
        QVERIFY2(qAbs(qRed(got) - qRed(color)) <= 4, qPrintable(detail));
        QVERIFY2(qAbs(qGreen(got) - qGreen(color)) <= 4, qPrintable(detail));
        QVERIFY2(qAbs(qBlue(got) - qBlue(color)) <= 4, qPrintable(detail));
    }

    QVERIFY(lanrdp::nv12ToArgb32(nullptr, 8, 8, 8).isNull());
}

// ---------------------------------------------------------------------------
// nv12ToArgb32Scaled
// ---------------------------------------------------------------------------

void TestColorConvert::scaledConversionFitsRequestedBox()
{
    // 1920x1080 塞进 480x360 的框：等比因子取较小的那个 = 480/1920 = 0.25，
    // 于是输出 480x270（高度跟着比例走，不是把框填满到 360）
    const QByteArray src  = makeBgra(1920, 1080, solid(1920, 1080, qRgb(30, 60, 90)));
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(src.constData()), 1920 * 4, 1920, 1080);
    QVERIFY(!nv12.isEmpty());

    const QImage image = lanrdp::nv12ToArgb32Scaled(
        reinterpret_cast<const uchar *>(nv12.constData()), 1920, 1920, 1080, 480, 360);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(480, 270));
}

void TestColorConvert::scaledConversionKeepsAspectRatio()
{
    // 高比宽更受限时，宽也不能超出框：1920x1080 -> 500x500 的框 => 500x281
    const QByteArray src  = makeBgra(1920, 1080, solid(1920, 1080, qRgb(200, 100, 50)));
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(src.constData()), 1920 * 4, 1920, 1080);

    const QImage image = lanrdp::nv12ToArgb32Scaled(
        reinterpret_cast<const uchar *>(nv12.constData()), 1920, 1920, 1080, 500, 500);
    QVERIFY(!image.isNull());

    QVERIFY2(image.width() <= 500 && image.height() <= 500, "输出必须完全落在框内");

    // 宽高比必须与源一致（只允许四舍五入带来的极小偏差）——
    // 渲染器的 mapToNormalized 走 scaledTargetRect，靠的就是「等比」这一前提
    const double srcRatio = 1920.0 / 1080.0;
    const double dstRatio = double(image.width()) / double(image.height());
    QVERIFY2(qAbs(srcRatio - dstRatio) < 0.01,
             qPrintable(QStringLiteral("ratio %1 vs %2").arg(srcRatio).arg(dstRatio)));
}

void TestColorConvert::scaledConversionNeverUpscales()
{
    // 目标框比源大得多时不做放大（放大交给 QPainter，别白烧一次内存写）
    const QByteArray src  = makeBgra(320, 240, solid(320, 240, qRgb(10, 20, 30)));
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(src.constData()), 320 * 4, 320, 240);

    const QImage image = lanrdp::nv12ToArgb32Scaled(
        reinterpret_cast<const uchar *>(nv12.constData()), 320, 320, 240, 3840, 2160);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(320, 240));
}

void TestColorConvert::scaledConversionSamplesSourceRegion()
{
    // 8x8：左半红、右半蓝。缩到 8x2 -> 2x2 时，左列必取到红、右列必取到蓝
    QVector<QRgb> pixels(8 * 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            pixels[y * 8 + x] = (x < 4) ? qRgb(255, 0, 0) : qRgb(0, 0, 255);

    const QByteArray src  = makeBgra(8, 8, pixels);
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(src.constData()), 8 * 4, 8, 8);

    const QImage image = lanrdp::nv12ToArgb32Scaled(
        reinterpret_cast<const uchar *>(nv12.constData()), 8, 8, 8, 2, 2);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(2, 2));

    const QRgb left  = image.pixel(0, 0);
    const QRgb right = image.pixel(1, 0);
    QVERIFY2(qRed(left) > 200 && qBlue(left) < 60,
             qPrintable(QStringLiteral("left not red: #%1").arg(left, 8, 16, QLatin1Char('0'))));
    QVERIFY2(qBlue(right) > 200 && qRed(right) < 60,
             qPrintable(QStringLiteral("right not blue: #%1").arg(right, 8, 16, QLatin1Char('0'))));
}

void TestColorConvert::scaledConversionFallsBackOnDegenerateBox()
{
    const QByteArray src  = makeBgra(8, 8, solid(8, 8, qRgb(120, 130, 140)));
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(src.constData()), 8 * 4, 8, 8);
    const auto         p  = reinterpret_cast<const uchar *>(nv12.constData());

    // 目标框被压到 0/负 -> 回退原尺寸，绝不返回空图（否则画面会整块消失）
    QCOMPARE(lanrdp::nv12ToArgb32Scaled(p, 8, 8, 8, 0, 0).size(), QSize(8, 8));
    QCOMPARE(lanrdp::nv12ToArgb32Scaled(p, 8, 8, 8, -5, 20).size(), QSize(8, 8));

    // 源本身非法 -> 空图（与 nv12ToArgb32 口径一致）
    QVERIFY(lanrdp::nv12ToArgb32Scaled(nullptr, 8, 8, 8, 4, 4).isNull());
    QVERIFY(lanrdp::nv12ToArgb32Scaled(p, 8, 0, 8, 4, 4).isNull());
    QVERIFY(lanrdp::nv12ToArgb32Scaled(p, 4, 8, 8, 4, 4).isNull());  // stride 小于一行
}

void TestColorConvert::scaledConversionSkipsWhenNotWorthIt()
{
    // 收益守卫的**边界**：box 平均要给每个输出像素额外交「4 次取样 + 乘倒数」的开销，
    // 只有输出面积远小于源时才划算。实测（A2b 细扫，1080p 与 4K 曲线几乎重合）：
    // 面积比 1/4 -> 0.53x ｜ 1/6 -> 0.75x ｜ 1/8 -> 0.94x ｜ 1/9 -> 1.06x ｜ 1/16 -> 1.63x
    // ⇒ 盈亏平衡在 1/9，守卫取 `dstArea * 9 > srcArea` 就退回全尺寸。
    //
    // ⚠️ 这条用例的价值在**卡住阈值本身**：旧值是 1/4，而「1/2 线性尺寸」（面积比恰好 1/4）
    // 是曲线上的**最差点**（慢一倍），且 4K 流到 4K 屏做 2×2 格子时预算正好落在那里。
    const QByteArray src  = makeBgra(320, 240, solid(320, 240, qRgb(90, 140, 190)));
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(src.constData()), 320 * 4, 320, 240);
    const auto p = reinterpret_cast<const uchar *>(nv12.constData());

    // 面积比 0.88（轻微缩小）-> 不缩，返回原尺寸
    QCOMPARE(lanrdp::nv12ToArgb32Scaled(p, 320, 320, 240, 300, 226).size(), QSize(320, 240));

    // **面积比恰好 1/4**（160x120，= 1/2 线性尺寸）-> 不缩
    // —— 旧守卫写 `> 4 * srcArea` 时这一点会走缩放路径，正是被修掉的那个反向异常
    QCOMPARE(lanrdp::nv12ToArgb32Scaled(p, 320, 320, 240, 160, 120).size(), QSize(320, 240));

    // 面积比 1/7.1（120x90，仍在阈值之上）-> 不缩
    QCOMPARE(lanrdp::nv12ToArgb32Scaled(p, 320, 320, 240, 120, 90).size(), QSize(320, 240));

    // 面积比 1/16（80x60，明显值得）-> 真缩
    QCOMPARE(lanrdp::nv12ToArgb32Scaled(p, 320, 320, 240, 80, 60).size(), QSize(80, 60));
}

void TestColorConvert::scaledConversionThresholdBoundaryIsInclusive()
{
    // 阈值是「面积比 ≤ 1/9 就缩」，所以**恰好 1/9** 必须缩（实测 1.06x，仍赚且输出更小）。
    // 用 240x240 → 80x80（= 1/9 面积，整数无取整误差）卡死这条边界。
    const QByteArray src  = makeBgra(240, 240, solid(240, 240, qRgb(200, 120, 40)));
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(src.constData()), 240 * 4, 240, 240);
    const auto p = reinterpret_cast<const uchar *>(nv12.constData());

    QCOMPARE(lanrdp::nv12ToArgb32Scaled(p, 240, 240, 240, 80, 80).size(), QSize(80, 80));

    // 差一点点（81x81 -> 面积比 0.114 > 1/9）就该回退，说明边界是**卡在 1/9** 而不是更宽
    QCOMPARE(lanrdp::nv12ToArgb32Scaled(p, 240, 240, 240, 81, 81).size(), QSize(240, 240));
}

QTEST_GUILESS_MAIN(TestColorConvert)
#include "test_colorconvert.moc"
