#include <QtTest/QtTest>

#include <QByteArray>
#include <QVector>

#include "protocol/ScreenControl.h"

// 多屏两帧（协议 v5：SCREEN_LIST = 19 / SCREEN_SELECT = 20）的编解码。
//
// 线上字段一律不可信：解码侧要拿 count 去分配、拿宽高去推几何，
// 越界就是巨额分配或界面上一条不可能的屏。所以每条拒绝分支都要有用例。
class TestScreenControl : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsSingleMonitor();
    void roundTripsMultipleMonitorsWithNegativeOrigin();
    void roundTripsMaximumCount();
    void rejectsEmptyList();
    void rejectsAbsurdCount();
    void rejectsTruncatedPayload();
    void rejectsTrailingGarbage();
    void rejectsWrongListVersion();
    void rejectsListWithoutExactlyOnePrimary();
    void rejectsZeroSizedOrAbsurdGeometry();
    void rejectsCurrentOutOfRange();
    void encodesNothingForAnInvalidList();

    void roundTripsSelect();
    void rejectsBadSelect();
};

namespace {

lanrdp::ScreenInfo screen(qint32 x, qint32 y, quint32 w, quint32 h, bool primary = false)
{
    lanrdp::ScreenInfo s;
    s.x       = x;
    s.y       = y;
    s.width   = w;
    s.height  = h;
    s.primary = primary;
    return s;
}

} // namespace

void TestScreenControl::roundTripsSingleMonitor()
{
    lanrdp::ScreenList list;
    list.screens.append(screen(0, 0, 3840, 2160, true));
    list.current = 0;

    const QByteArray payload = lanrdp::encodeScreenList(list);
    QVERIFY(!payload.isEmpty());
    // ver + count + current + 1 × 17
    QCOMPARE(payload.size(), qsizetype(3 + 17));

    const auto decoded = lanrdp::decodeScreenList(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->screens.size(), 1);
    QCOMPARE(decoded->screens.at(0).width, quint32(3840));
    QCOMPARE(decoded->screens.at(0).height, quint32(2160));
    QVERIFY(decoded->screens.at(0).primary);
    QCOMPARE(decoded->current, quint8(0));
    QVERIFY(decoded->currentScreen() != nullptr);
    QCOMPARE(decoded->currentScreen()->width, quint32(3840));
}

// 非对称排布（一块在主屏左侧、一块在上方）时 x / y 是**负数** —— 用无符号编码
// 会把它们变成天文数字，注入坐标随即全错。
void TestScreenControl::roundTripsMultipleMonitorsWithNegativeOrigin()
{
    lanrdp::ScreenList list;
    list.screens.append(screen(-1920, -1080, 1920, 1080));       // 左上
    list.screens.append(screen(0, 0, 2560, 1440, true));         // 主屏
    list.screens.append(screen(2560, 200, 1920, 1080));          // 右侧
    list.current = 2;

    const auto decoded = lanrdp::decodeScreenList(lanrdp::encodeScreenList(list));
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->screens.size(), 3);
    QCOMPARE(decoded->screens.at(0).x, qint32(-1920));
    QCOMPARE(decoded->screens.at(0).y, qint32(-1080));
    QCOMPARE(decoded->screens.at(1).x, qint32(0));
    QVERIFY(decoded->screens.at(1).primary);
    QCOMPARE(decoded->screens.at(2).x, qint32(2560));
    QCOMPARE(decoded->screens.at(2).y, qint32(200));
    QCOMPARE(decoded->current, quint8(2));
    QCOMPARE(decoded->currentScreen()->x, qint32(2560));
}

void TestScreenControl::roundTripsMaximumCount()
{
    lanrdp::ScreenList list;
    for (int i = 0; i < lanrdp::kMaxScreens; ++i)
        list.screens.append(screen(i * 100, 0, 100, 100, i == 0));
    list.current = 3;

    const auto decoded = lanrdp::decodeScreenList(lanrdp::encodeScreenList(list));
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->screens.size(), int(lanrdp::kMaxScreens));
    QCOMPARE(decoded->current, quint8(3));
}

void TestScreenControl::rejectsEmptyList()
{
    lanrdp::ScreenList list;   // 0 块屏
    QVERIFY(lanrdp::encodeScreenList(list).isEmpty());

    // 手工构造一份 count = 0 的载荷：必须拒绝，而不是给一个「没有屏」的空状态
    // （控制端界面会因此只剩一个无项的菜单，且 current 无从解释）。
    QByteArray wire;
    wire.append(char(lanrdp::kScreenControlVersion));
    wire.append(char(0));
    wire.append(char(0));
    QVERIFY(!lanrdp::decodeScreenList(wire).has_value());
}

void TestScreenControl::rejectsAbsurdCount()
{
    QByteArray wire;
    wire.append(char(lanrdp::kScreenControlVersion));
    wire.append(char(200));   // 远超 kMaxScreens
    wire.append(char(0));
    QVERIFY(!lanrdp::decodeScreenList(wire).has_value());
}

void TestScreenControl::rejectsTruncatedPayload()
{
    lanrdp::ScreenList list;
    list.screens.append(screen(0, 0, 1920, 1080, true));
    const QByteArray full = lanrdp::encodeScreenList(list);
    QVERIFY(full.size() > 3);

    for (qsizetype cut = 1; cut < full.size(); ++cut) {
        QVERIFY2(!lanrdp::decodeScreenList(full.left(cut)).has_value(),
                 qPrintable(QStringLiteral("截断到 %1 字节仍被接受").arg(cut)));
    }
    QVERIFY(!lanrdp::decodeScreenList(QByteArray()).has_value());
}

// 多出来的字节说明两端对布局的认知已经不一致了（与 AUDIO_CONFIG 同一口径）。
void TestScreenControl::rejectsTrailingGarbage()
{
    lanrdp::ScreenList list;
    list.screens.append(screen(0, 0, 1920, 1080, true));
    QByteArray wire = lanrdp::encodeScreenList(list);
    wire.append(char(0));
    QVERIFY(!lanrdp::decodeScreenList(wire).has_value());
}

// 版本号是这一层自己的（与帧头 Version 无关）：将来加字段时靠它区分，
// 认不出来就必须拒，不能「近似解析」。
void TestScreenControl::rejectsWrongListVersion()
{
    lanrdp::ScreenList list;
    list.screens.append(screen(0, 0, 1920, 1080, true));
    QByteArray wire = lanrdp::encodeScreenList(list);
    wire[0] = char(lanrdp::kScreenControlVersion + 1);
    QVERIFY(!lanrdp::decodeScreenList(wire).has_value());
}

void TestScreenControl::rejectsListWithoutExactlyOnePrimary()
{
    // 没有主屏
    lanrdp::ScreenList none;
    none.screens.append(screen(0, 0, 1920, 1080, false));
    none.screens.append(screen(1920, 0, 1920, 1080, false));
    QVERIFY(lanrdp::encodeScreenList(none).isEmpty());

    // 两个主屏
    lanrdp::ScreenList two;
    two.screens.append(screen(0, 0, 1920, 1080, true));
    two.screens.append(screen(1920, 0, 1920, 1080, true));
    QVERIFY(lanrdp::encodeScreenList(two).isEmpty());

    // 解码侧同样要拒（对端可能不按规矩来）
    QByteArray wire;
    wire.append(char(lanrdp::kScreenControlVersion));
    wire.append(char(1));
    wire.append(char(0));
    wire.append(QByteArray(4, 0));                       // x
    wire.append(QByteArray(4, 0));                       // y
    wire.append(QByteArray::fromHex("80070000"));        // w = 1920
    wire.append(QByteArray::fromHex("38040000"));        // h = 1080
    wire.append(char(0));                                // primary = false
    QVERIFY(!lanrdp::decodeScreenList(wire).has_value());
}

void TestScreenControl::rejectsZeroSizedOrAbsurdGeometry()
{
    lanrdp::ScreenList zero;
    zero.screens.append(screen(0, 0, 0, 1080, true));
    QVERIFY(lanrdp::encodeScreenList(zero).isEmpty());

    lanrdp::ScreenList huge;
    huge.screens.append(screen(0, 0, lanrdp::kMaxScreenDimension + 1, 1080, true));
    QVERIFY(lanrdp::encodeScreenList(huge).isEmpty());

    // 坐标也不能是天文数字（否则归一化坐标会被算成一个荒谬的点）
    lanrdp::ScreenList far;
    far.screens.append(screen(1000000, 0, 1920, 1080, true));
    QVERIFY(lanrdp::encodeScreenList(far).isEmpty());
}

void TestScreenControl::rejectsCurrentOutOfRange()
{
    lanrdp::ScreenList list;
    list.screens.append(screen(0, 0, 1920, 1080, true));
    list.current = 5;
    QVERIFY(!lanrdp::isValidScreenList(list));
    QVERIFY(lanrdp::encodeScreenList(list).isEmpty());

    QByteArray wire;
    wire.append(char(lanrdp::kScreenControlVersion));
    wire.append(char(1));
    wire.append(char(7));                          // current 越界
    wire.append(QByteArray(4, 0));
    wire.append(QByteArray(4, 0));
    wire.append(QByteArray::fromHex("80070000"));
    wire.append(QByteArray::fromHex("38040000"));
    wire.append(char(1));
    QVERIFY(!lanrdp::decodeScreenList(wire).has_value());
}

void TestScreenControl::encodesNothingForAnInvalidList()
{
    // encode 对非法输入一律回空数组（= 拒发），与 Frame::serialize 同一口径：
    // 宁可什么都不发，也不要发出一份自相矛盾的状态。
    QVERIFY(lanrdp::encodeScreenList(lanrdp::ScreenList()).isEmpty());
}

void TestScreenControl::roundTripsSelect()
{
    for (int i = 0; i < lanrdp::kMaxScreens; ++i) {
        const QByteArray payload = lanrdp::encodeScreenSelect(static_cast<quint8>(i));
        QCOMPARE(payload.size(), lanrdp::kScreenSelectSize);
        const auto index = lanrdp::decodeScreenSelect(payload);
        QVERIFY(index.has_value());
        QCOMPARE(*index, static_cast<quint8>(i));
    }

    // 超范围的 index 根本不发（控制端界面只可能点列表里有的项）
    QVERIFY(lanrdp::encodeScreenSelect(lanrdp::kMaxScreens).isEmpty());
}

void TestScreenControl::rejectsBadSelect()
{
    lanrdp::ScreenList list;
    list.screens.append(screen(0, 0, 1920, 1080, true));
    QByteArray wire = lanrdp::encodeScreenSelect(0);

    QVERIFY(!lanrdp::decodeScreenSelect(QByteArray()).has_value());
    QVERIFY(!lanrdp::decodeScreenSelect(wire.left(1)).has_value());
    QVERIFY(!lanrdp::decodeScreenSelect(wire + char(0)).has_value());

    QByteArray wrongVersion = wire;
    wrongVersion[0] = char(lanrdp::kScreenControlVersion + 1);
    QVERIFY(!lanrdp::decodeScreenSelect(wrongVersion).has_value());

    QVERIFY(!lanrdp::decodeScreenSelect(lanrdp::encodeScreenSelect(lanrdp::kMaxScreens)).has_value());
}

QTEST_GUILESS_MAIN(TestScreenControl)
#include "test_screencontrol.moc"
