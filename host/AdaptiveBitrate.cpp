#include "host/AdaptiveBitrate.h"

namespace lanrdp {

AdaptiveState adaptStep(qint64 pendingBytes, const AdaptiveState &current,
                        const AdaptiveParams &params)
{
    AdaptiveState next = current;

    if (pendingBytes > params.highWatermark) {
        // 卡：先降码率，码率到下限再降帧率。
        // 码率不可调时（裸帧）跳过降价分支，直接动帧率。
        if (params.bitrateAdjustable && current.bitrateBps > params.minBitrateBps)
            next.bitrateBps = qMax(params.minBitrateBps, current.bitrateBps / 2);
        else if (current.fps > params.minFps)
            next.fps = qMax(params.minFps, current.fps / 2);
        // 双下限：保持（已无可降）。
    } else if (pendingBytes < params.lowWatermark) {
        // 宽松：先恢复帧率，帧率满再缓慢升码率。
        if (current.fps < params.maxFps) {
            next.fps = qMin(params.maxFps, current.fps * 2);
        } else if (params.bitrateAdjustable && current.bitrateBps < params.maxBitrateBps) {
            next.bitrateBps = qMin(params.maxBitrateBps,
                                   current.bitrateBps + current.bitrateBps / 10);
        }
        // 满状态：保持。
    }
    // 中间区间：保持（迟滞）。

    return next;
}

AdaptiveParams deriveAdaptiveParams(int userBitrateBps, int baseFps)
{
    AdaptiveParams p;

    // ① 用户设的码率就是上限；给一个地板，避免 0 / 负数把码率压到不可用。
    p.maxBitrateBps = qMax(100000, userBitrateBps);

    // ② 下限不得超过上限 —— 否则「先降码率」这一路会被自己短路。
    if (p.minBitrateBps > p.maxBitrateBps)
        p.minBitrateBps = p.maxBitrateBps;

    // 帧率上限 = 基准帧率（用户设定）；地板 1 只用来挡 0 / 负数。
    // ⚠️ 不能把下限 5 当下限来用：基准帧率 < 5 时那会把帧率**抬到** 5，
    // 与「只在用户设定以内调节」相反。上下限打架时让下限退让（同 ② 的码率）。
    p.maxFps = qMax(1, baseFps);
    if (p.minFps > p.maxFps)
        p.minFps = p.maxFps;

    return p;
}

} // namespace lanrdp
