#pragma once

#include <QtGlobal>

namespace lanrdp {

// 自适应码率 / 帧率：可调参数。
// 高 / 低水位是「发送队列积压（socket bytesToWrite）」的阈值；高水位之上判「卡」，
// 低水位之下判「宽松」，两者之间保持（迟滞，防振荡）。
struct AdaptiveParams
{
    int    minBitrateBps = 500000;           // 500 kbps
    int    maxBitrateBps = 8 * 1000 * 1000;  // 8 Mbps
    int    minFps        = 5;
    int    maxFps        = 30;
    qint64 highWatermark = 2 * 1024 * 1024;  // 2 MB
    qint64 lowWatermark  = 512 * 1024;       // 512 KB

    // 码率是否可调。裸帧路径没有 setBitrate —— 对它而言「降码率」是 no-op，
    // 若仍走降价分支就会白等若干个 tick 才轮到降帧率，故置 false 让
    // adaptStep 直接动帧率。默认 true（H.264 路径行为不变）。
    bool bitrateAdjustable = true;
};

struct AdaptiveState
{
    int bitrateBps;
    int fps;
};

// 纯函数：据当前积压与状态，算出下一步码率 / 帧率。
//
// 保流畅（不卡）优先 —— 降快、升慢：
// · 积压 > 高水位：先码率减半；码率已到下限仍积压，再帧率减半。
// · 积压 < 低水位：先帧率翻倍（若降过）；帧率已满再码率 +10%。
// · 中间区间：保持。
// 返回值不越过 min/max 边界。
AdaptiveState adaptStep(qint64 pendingBytes, const AdaptiveState &current,
                        const AdaptiveParams &params);

// 由「用户设定」导出边界。两条不变式：
// ① maxBitrateBps == 用户设的目标码率（地板 100 kbps）
// —— 自适应**只允许在用户设定以下调节**，绝不上探；
// ② minBitrateBps = min(默认下限, maxBitrateBps)
// —— 用户设得比默认下限还低时下限必须跟着降，否则 `adaptStep` 里
// 「先降码率」这一路会在 cur == 用户值 时被短路，变成直接砍帧率。
// maxFps = max(默认下限, baseFps)，保证上下限不打架。
AdaptiveParams deriveAdaptiveParams(int userBitrateBps, int baseFps);

} // namespace lanrdp
