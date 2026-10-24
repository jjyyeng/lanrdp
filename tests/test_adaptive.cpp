#include <QtTest/QtTest>

#include "host/AdaptiveBitrate.h"

// 自适应码率策略纯函数。
// 覆盖：降码率 / 码率到下限降帧率 / 恢复帧率 / 恢复码率 / 迟滞保持 / 双下限 / 边界钳制。
class TestAdaptive : public QObject
{
    Q_OBJECT

private slots:
    void highWatermarkHalvesBitrate();
    void atMinBitrateHighWatermarkHalvesFps();
    void lowWatermarkRestoresFpsFirst();
    void fullFpsLowWatermarkRaisesBitrate();
    void midRangeKeepsState();
    void bothAtFloorHighWatermarkKeeps();
    void bitrateClampedToMin();

    // 由「用户设定」导出边界（防反向覆盖）
    void derivedParamsNeverRaiseBitrateAboveUserValue();
    void derivedParamsKeepFloorBelowCeiling();
    void derivedParamsNeverRaiseFpsAboveUserValue();
    void derivedParamsCongestionDropsBitrateFirst();

    // 码率不可调的路径（裸帧）：拥塞直接降帧率、恢复直接升帧率，不空转
    void unadjustableBitrateDropsFpsImmediately();
    void unadjustableBitrateRestoresFpsImmediately();
};

namespace {

const lanrdp::AdaptiveParams &p()
{
    static const lanrdp::AdaptiveParams params;  // 用默认值：H=2MB L=512KB，码率[0.5M,8M]，fps[5,30]
    return params;
}

constexpr qint64 kHigh = 3 * 1024 * 1024;  // > 2MB
constexpr qint64 kLow  = 100 * 1024;       // < 512KB
constexpr qint64 kMid  = 1024 * 1024;      // 512KB ~ 2MB 之间

} // namespace

void TestAdaptive::highWatermarkHalvesBitrate()
{
    const lanrdp::AdaptiveState cur{8000000, 30};
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kHigh, cur, p());
    QCOMPARE(next.bitrateBps, 4000000);
    QCOMPARE(next.fps, 30);
}

void TestAdaptive::atMinBitrateHighWatermarkHalvesFps()
{
    const lanrdp::AdaptiveState cur{500000, 30};  // 码率已到下限
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kHigh, cur, p());
    QCOMPARE(next.bitrateBps, 500000);
    QCOMPARE(next.fps, 15);
}

void TestAdaptive::lowWatermarkRestoresFpsFirst()
{
    const lanrdp::AdaptiveState cur{500000, 15};  // 降过帧率、码率在低处
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kLow, cur, p());
    QCOMPARE(next.fps, 30);         // 先恢复帧率
    QCOMPARE(next.bitrateBps, 500000);  // 码率不动
}

void TestAdaptive::fullFpsLowWatermarkRaisesBitrate()
{
    const lanrdp::AdaptiveState cur{500000, 30};  // 帧率已满
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kLow, cur, p());
    QCOMPARE(next.fps, 30);
    QCOMPARE(next.bitrateBps, 550000);  // +10%
}

void TestAdaptive::midRangeKeepsState()
{
    const lanrdp::AdaptiveState cur{4000000, 30};
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kMid, cur, p());
    QCOMPARE(next.bitrateBps, 4000000);  // 迟滞：不升不降
    QCOMPARE(next.fps, 30);
}

void TestAdaptive::bothAtFloorHighWatermarkKeeps()
{
    const lanrdp::AdaptiveState cur{500000, 5};  // 双下限
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kHigh, cur, p());
    QCOMPARE(next.bitrateBps, 500000);
    QCOMPARE(next.fps, 5);
}

void TestAdaptive::bitrateClampedToMin()
{
    const lanrdp::AdaptiveState cur{600000, 30};  // 600k 减半 = 300k，但下限 500k
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kHigh, cur, p());
    QCOMPARE(next.bitrateBps, 500000);  // 钳到 min，而非 300k
    QCOMPARE(next.fps, 30);
}

// ---------------------------------------------------------------------------
// 边界必须**由用户设定导出**。
//
// 旧实现：`maxBitrateBps = qMax(500000, h264Bitrate_)`，而 `minBitrateBps` 保持默认 500000。
// 用户设 `--bitrate 200000` 时 → max 被抬到 500k（比用户值高 2.5 倍），min 也是 500k
// ⇒ ①「宽松」时码率会一路爬到 500k，**越过用户设定**；
// ②「拥塞」时 `cur(200k) > min(500k)` 为假，于是**跳过降码率直接砍帧率**。
// 两条都是「自适应反过来覆盖用户意图」。
// ---------------------------------------------------------------------------

void TestAdaptive::derivedParamsNeverRaiseBitrateAboveUserValue()
{
    const lanrdp::AdaptiveParams low = lanrdp::deriveAdaptiveParams(200000, 30);

    QCOMPARE(low.maxBitrateBps, 200000);   // 上限就是用户值，不抬到 500k

    // 宽松（积压低于低水位）时再怎么恢复，也不得超过用户设定的 200 kbps
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kLow, {200000, 30}, low);
    QVERIFY2(next.bitrateBps <= 200000, "自适应绝不能把码率抬到用户设定之上");
}

void TestAdaptive::derivedParamsKeepFloorBelowCeiling()
{
    const lanrdp::AdaptiveParams low = lanrdp::deriveAdaptiveParams(200000, 30);
    QVERIFY2(low.minBitrateBps <= low.maxBitrateBps,
             "下限不得高于上限（否则降码率这一路会被自己短路）");
    QCOMPARE(low.minBitrateBps, 200000);   // 用户设得比默认下限低，下限跟着降

    // 正常用户值：下限仍是默认 500k，上限是用户值
    const lanrdp::AdaptiveParams normal = lanrdp::deriveAdaptiveParams(8000000, 30);
    QCOMPARE(normal.minBitrateBps, 500000);
    QCOMPARE(normal.maxBitrateBps, 8000000);
    QCOMPARE(normal.maxFps, 30);
}

// 帧率同理：用户设的基准就是上限。下限 5 是「降到底线」，不是「抬到底线」——
// 基准 2 fps 时把上限算成 5，会让自适应把帧率**抬到用户设定之上**。
void TestAdaptive::derivedParamsNeverRaiseFpsAboveUserValue()
{
    const lanrdp::AdaptiveParams slow = lanrdp::deriveAdaptiveParams(8000000, 2);
    QCOMPARE(slow.maxFps, 2);
    QVERIFY2(slow.minFps <= slow.maxFps, "下限不得高于上限（否则降帧率这一路会被自己短路）");

    // 宽松时再怎么恢复，也不得超过用户设的 2 fps
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kLow, {8000000, 2}, slow);
    QVERIFY2(next.fps <= 2, "自适应绝不能把帧率抬到用户设定之上");
}

void TestAdaptive::derivedParamsCongestionDropsBitrateFirst()
{
    // 用正常用户值导出：拥塞时必须**先降码率**（不是直接砍帧率）
    const lanrdp::AdaptiveParams normal = lanrdp::deriveAdaptiveParams(8000000, 30);
    const lanrdp::AdaptiveState  cut    = lanrdp::adaptStep(kHigh, {8000000, 30}, normal);
    QCOMPARE(cut.bitrateBps, 4000000);
    QCOMPARE(cut.fps, 30);

    // 用户把码率压得很低时：它同时是下限，此时才轮到砍帧率
    const lanrdp::AdaptiveParams low  = lanrdp::deriveAdaptiveParams(200000, 30);
    const lanrdp::AdaptiveState  cut2 = lanrdp::adaptStep(kHigh, {200000, 30}, low);
    QCOMPARE(cut2.bitrateBps, 200000);   // 不再往下（尊重用户设定）
    QCOMPARE(cut2.fps, 15);
}

// 码率不可调（裸帧没有 setBitrate）时，拥塞必须**直接降帧率**，
// 不能先在「降价」分支上空转若干 tick 才轮到帧率。
void TestAdaptive::unadjustableBitrateDropsFpsImmediately()
{
    lanrdp::AdaptiveParams raw = p();
    raw.bitrateAdjustable = false;

    // 即便当前码率远高于下限，也不该动码率；一个 tick 就砍帧率。
    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kHigh, {8000000, 30}, raw);
    QCOMPARE(next.bitrateBps, 8000000);  // 码率不动
    QCOMPARE(next.fps, 15);              // 帧率立刻减半
}

// 恢复时同理：帧率优先回升；码率不可调 ⇒ 不碰码率。
void TestAdaptive::unadjustableBitrateRestoresFpsImmediately()
{
    lanrdp::AdaptiveParams raw = p();
    raw.bitrateAdjustable = false;

    const lanrdp::AdaptiveState next = lanrdp::adaptStep(kLow, {8000000, 10}, raw);
    QCOMPARE(next.bitrateBps, 8000000);  // 码率不动
    QCOMPARE(next.fps, 20);              // 帧率翻倍回升
}

QTEST_GUILESS_MAIN(TestAdaptive)
#include "test_adaptive.moc"
