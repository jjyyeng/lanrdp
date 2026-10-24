#include <QtTest/QtTest>

#include <QPoint>
#include <QSize>

#include "viewer/ScreenRenderer.h"

// 控件坐标 -> 画面归一化坐标（0..65535）的纯映射
class TestInputMap : public QObject
{
    Q_OBJECT

private slots:
    void mapsFrameCenterToMiddle();
    void mapsEdgesToExtremes();
    void clampsPointsInLetterboxBars();
    void rejectsInvalidGeometry();
    void isAspectRatioAware();
};

namespace {

// 16:9 画面显示在 4:3 控件里 -> 上下留黑边，target 为 (0,75,800,450)
constexpr int kWidgetW = 800;
constexpr int kWidgetH = 600;
const QSize   kFrame(1920, 1080);

// 归一化坐标容差。端点用的是 [origin, origin+size-1] -> 0..65535 的约定，所以
// 「控件中心」并不正好落在 32768：本组参数下 nx≈32808（偏 +40）、ny≈32840（偏 +72）。
// 取 100 仍比原先的 200 收紧一半，足以拦住「letterbox 偏移 / 尺寸算错」这类成百上千
// 单位的系统性错误，又不至于把上述合法偏差误判为失败。
constexpr int kTolerance = 100;

quint16 mappedX(const QPoint &pos)
{
    quint16 nx = 0;
    quint16 ny = 0;
    const bool ok = lanrdp::ScreenRenderer::mapToNormalized(
        pos, kFrame, QSize(kWidgetW, kWidgetH), &nx, &ny);
    // 不能用 Q_ASSERT —— release（QT_NO_DEBUG）下它是空语句，几何退化会被静默放过。
    // helper 返回 quint16，也用不了 QVERIFY（展开成 `return;`），故用 qVerify。
    QTest::qVerify(ok, "ScreenRenderer::mapToNormalized()",
                   "mapToNormalized() 对合法几何返回了 false", __FILE__, __LINE__);
    return nx;
}

quint16 mappedY(const QPoint &pos)
{
    quint16 nx = 0;
    quint16 ny = 0;
    const bool ok = lanrdp::ScreenRenderer::mapToNormalized(
        pos, kFrame, QSize(kWidgetW, kWidgetH), &nx, &ny);
    QTest::qVerify(ok, "ScreenRenderer::mapToNormalized()",
                   "mapToNormalized() 对合法几何返回了 false", __FILE__, __LINE__);
    return ny;
}

} // namespace

void TestInputMap::mapsFrameCenterToMiddle()
{
    // 控件中心 (400,300) 恰好是画面中心（letterbox 上下各 75px 是对称的）
    const quint16 nx = mappedX(QPoint(400, 300));
    const quint16 ny = mappedY(QPoint(400, 300));

    QVERIFY2(qAbs(int(nx) - 32768) <= kTolerance,
             qPrintable(QStringLiteral("nx=%1 偏离中心过多").arg(nx)));
    QVERIFY2(qAbs(int(ny) - 32768) <= kTolerance,
             qPrintable(QStringLiteral("ny=%1 偏离中心过多").arg(ny)));
}

void TestInputMap::mapsEdgesToExtremes()
{
    // target 矩形是 (0,75,800,450)：左/上 = 画面原点，右/下 = 右下角
    QCOMPARE(mappedX(QPoint(0, 300)), quint16(0));
    QCOMPARE(mappedY(QPoint(400, 75)), quint16(0));

    QCOMPARE(mappedX(QPoint(799, 300)), quint16(65535));
    QCOMPARE(mappedY(QPoint(400, 524)), quint16(65535));
}

void TestInputMap::clampsPointsInLetterboxBars()
{
    // 上黑边（y < 75）与下黑边（y > 524）都应钳到画面边界，而不是产生越界坐标
    QCOMPARE(mappedY(QPoint(400, 0)), quint16(0));
    QCOMPARE(mappedY(QPoint(400, 74)), quint16(0));
    QCOMPARE(mappedY(QPoint(400, 599)), quint16(65535));

    // 左右越界同理
    QCOMPARE(mappedX(QPoint(-500, 300)), quint16(0));
    QCOMPARE(mappedX(QPoint(5000, 300)), quint16(65535));
}

void TestInputMap::rejectsInvalidGeometry()
{
    quint16 nx = 0;
    quint16 ny = 0;

    QVERIFY(!lanrdp::ScreenRenderer::mapToNormalized(
        QPoint(10, 10), QSize(), QSize(800, 600), &nx, &ny));
    QVERIFY(!lanrdp::ScreenRenderer::mapToNormalized(
        QPoint(10, 10), kFrame, QSize(0, 0), &nx, &ny));
}

void TestInputMap::isAspectRatioAware()
{
    // 画面被等比缩放到 (0,75,800,450)。若映射忽略了 letterbox 偏移，
    // 控件上边缘会被当成画面上边缘，下面的断言就会失败。
    const quint16 nyTop = mappedY(QPoint(400, 75));      // 画面上边缘
    const quint16 nyMid = mappedY(QPoint(400, 300));     // 画面中心
    QVERIFY(nyTop < nyMid);
    QCOMPARE(nyTop, quint16(0));

    // 纵向 75..524 共 450 行，映射到 0..65535 应近似线性
    const int expectedHalf = 65535 / 2;
    QVERIFY(qAbs(int(nyMid) - expectedHalf) <= kTolerance);
}

QTEST_MAIN(TestInputMap)
#include "test_inputmap.moc"
