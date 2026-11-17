#include <QtTest>

#include <QStringList>

#include "app/QualityReport.h"

// 质量读数与「一句话诊断」的纯函数部分。
//
// 这些断言守的是**不要误报**：一个天天喊「网络延迟偏高」的读数比没有读数更糟，
// 用户会学会无视它，等到真出事那天也一样无视。所以这里钉的几条都是
// 「这种情况不该报」以及「这种情况必须报」。
class TestQualityReport : public QObject
{
    Q_OBJECT

private slots:
    void noFramesMeansNoData();
    void staticScreenIsNotMistakenForNoData();
    void highRttIsBlamedOnTheNetwork();
    void slowDecodeIsBlamedOnThisMachine();
    void fourKDecodeIsNormalNotASlowdown();
    void slowDecodeAtLowFrameRateIsNotBlamed();
    void networkIsCheckedBeforeDecode();
    void missingReadingsDoNotProduceAnAlarm();
    void hudShowsEveryNumber();
    void hudMarksReadingsItDoesNotHave();
    void hostHudLeavesOutViewerOnlyRows();
    void verdictTextsAreDistinctAndNonEmpty();
};

namespace {

lanrdp::QualitySample healthyViewer()
{
    lanrdp::QualitySample s;
    s.frames   = 1234;
    s.fps      = 30.0;
    s.mbps     = 8.5;
    s.rttMs    = 3;
    s.decodeMs = 4.2;
    s.size     = QSize(1920, 1080);
    return s;
}

} // namespace

void TestQualityReport::noFramesMeansNoData()
{
    lanrdp::QualitySample s;
    s.rttMs = 2;   // 连上了但还没出画面
    QCOMPARE(lanrdp::judgeQuality(s), lanrdp::QualityVerdict::NoData);
}

// 静止的桌面几乎不发帧 —— 这是正常状态，不是故障。
// 用 fps == 0 判「没有画面」会让人每次不动鼠标就看到一次误报。
void TestQualityReport::staticScreenIsNotMistakenForNoData()
{
    lanrdp::QualitySample s = healthyViewer();
    s.fps  = 0.0;
    s.mbps = 0.0;
    QCOMPARE(lanrdp::judgeQuality(s), lanrdp::QualityVerdict::Good);
}

void TestQualityReport::highRttIsBlamedOnTheNetwork()
{
    lanrdp::QualitySample s = healthyViewer();
    s.rttMs = lanrdp::kSlowRttMs;   // 门槛上就算「偏高」，边界不含糊
    QCOMPARE(lanrdp::judgeQuality(s), lanrdp::QualityVerdict::NetworkSlow);
}

// 判据是「占了多少帧预算」，所以用例也必须按预算算 —— 直接写死毫秒数的话，
// 改门槛时这条会跟着实现一起错，等于没守。
void TestQualityReport::slowDecodeIsBlamedOnThisMachine()
{
    lanrdp::QualitySample s = healthyViewer();   // 30 fps ⇒ 每帧 33.3 ms
    s.decodeMs = 1000.0 / s.fps * lanrdp::kSlowDecodeShare;   // 正好一半
    QCOMPARE(lanrdp::judgeQuality(s), lanrdp::QualityVerdict::DecodeSlow);
}

// 4K 软解（解码 + NV12→ARGB，缩到窗口大小）在本机实测 12–18 ms/帧。
// 30 fps 的预算是 33 ms，这一档只占一半以内 —— 画面不卡就不该报警。
// （原先的判据是固定 12 ms，于是「看一眼 4K」就永久显示「本机解码吃紧」。）
void TestQualityReport::fourKDecodeIsNormalNotASlowdown()
{
    lanrdp::QualitySample s = healthyViewer();
    s.size     = QSize(3840, 2160);
    s.fps      = 30.0;
    s.decodeMs = 15.0;
    QCOMPARE(lanrdp::judgeQuality(s), lanrdp::QualityVerdict::Good);
}

// 静止桌面只发几帧，帧间隔被拉得很长；这时解码 15 ms 更不构成压力。
void TestQualityReport::slowDecodeAtLowFrameRateIsNotBlamed()
{
    lanrdp::QualitySample s = healthyViewer();
    s.fps      = 4.0;    // 250 ms 才一帧
    s.decodeMs = 15.0;
    QCOMPARE(lanrdp::judgeQuality(s), lanrdp::QualityVerdict::Good);
}

// 两项都差时必须有一个确定的答案（不能随枚举顺序或比较顺序改变）：
// 报网络 —— 因为换网线比换电脑更可能是对的建议。
void TestQualityReport::networkIsCheckedBeforeDecode()
{
    lanrdp::QualitySample s = healthyViewer();
    s.rttMs    = 200;
    s.decodeMs = 40.0;
    QCOMPARE(lanrdp::judgeQuality(s), lanrdp::QualityVerdict::NetworkSlow);
}

// 取不到读数时（老系统没有 TCP_INFO、裸帧路径没有解码器）不能顺势报警：
// 「不知道」与「不好」是两件事。
void TestQualityReport::missingReadingsDoNotProduceAnAlarm()
{
    lanrdp::QualitySample s = healthyViewer();
    s.rttMs    = -1;
    s.decodeMs = -1.0;
    QCOMPARE(lanrdp::judgeQuality(s), lanrdp::QualityVerdict::Good);
}

void TestQualityReport::hudShowsEveryNumber()
{
    const QString hud = lanrdp::qualityHudText(healthyViewer());
    QVERIFY2(hud.contains(QStringLiteral("1920x1080")), qPrintable(hud));
    QVERIFY2(hud.contains(QStringLiteral("30 fps")), qPrintable(hud));
    QVERIFY2(hud.contains(QStringLiteral("8.5 Mbps")), qPrintable(hud));
    QVERIFY2(hud.contains(QStringLiteral("3 ms")), qPrintable(hud));
    QVERIFY2(hud.contains(QStringLiteral("4.2 ms")), qPrintable(hud));
    QVERIFY2(hud.contains(QStringLiteral("1234")), qPrintable(hud));
    QVERIFY2(hud.contains(lanrdp::qualityVerdictText(lanrdp::QualityVerdict::Good)),
             qPrintable(hud));
}

// 没有的读数要显式标出来，不能省略那一行 —— 少了「往返」这一行，
// 读者会以为是界面太窄被截掉了。
void TestQualityReport::hudMarksReadingsItDoesNotHave()
{
    lanrdp::QualitySample s = healthyViewer();
    s.rttMs    = -1;
    s.decodeMs = -1.0;
    const QString hud = lanrdp::qualityHudText(s);
    QVERIFY2(hud.contains(QStringLiteral("—")), qPrintable(hud));
    QVERIFY2(!hud.contains(QStringLiteral(" ms")), qPrintable(hud));
}

// 被控端报的是发出去多少，不是收回来多少：往返与解码两行不该出现。
void TestQualityReport::hostHudLeavesOutViewerOnlyRows()
{
    lanrdp::QualitySample s = healthyViewer();
    s.hosting  = true;
    s.rttMs    = -1;
    s.decodeMs = -1.0;
    const QString hud = lanrdp::qualityHudText(s);
    QVERIFY2(!hud.contains(QStringLiteral("往返")), qPrintable(hud));
    QVERIFY2(!hud.contains(QStringLiteral("解码")), qPrintable(hud));
    QVERIFY2(hud.contains(QStringLiteral("30 fps")), qPrintable(hud));
}

void TestQualityReport::verdictTextsAreDistinctAndNonEmpty()
{
    const lanrdp::QualityVerdict all[] = {
        lanrdp::QualityVerdict::NoData,
        lanrdp::QualityVerdict::Good,
        lanrdp::QualityVerdict::NetworkSlow,
        lanrdp::QualityVerdict::DecodeSlow,
    };
    QStringList seen;
    for (lanrdp::QualityVerdict v : all) {
        const QString text = lanrdp::qualityVerdictText(v);
        QVERIFY2(!text.isEmpty(), "每种结论都要有一句话");
        QVERIFY2(!seen.contains(text), qPrintable(QStringLiteral("重复的结论文案：") + text));
        seen << text;
    }
}

QTEST_MAIN(TestQualityReport)
#include "test_qualityreport.moc"
