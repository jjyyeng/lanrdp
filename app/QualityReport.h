#pragma once

#include <QSize>
#include <QString>

namespace lanrdp {

// 状态栏悬停读数的一次采样。
//
// 每一项都有「不知道」的表示，且**故意**不用 0：0 在读数里都是合法值
//（回环往返 0 ms、静止画面 0 fps），拿 0 当「没有」会让读者得出相反结论。
struct QualitySample
{
    double  fps      = 0.0;    // 这一秒收到的帧数
    double  mbps     = 0.0;    // 这一秒的码率
    int     rttMs    = -1;     // 网络往返；-1 = 取不到
    double  decodeMs = -1.0;   // 解码耗时（ms/帧）；-1 = 不走解码 / 还没有样本
    quint64 frames   = 0;      // 累计帧数（0 = 还没有画面）
    QSize   size;              // 码流真实尺寸
    bool    hosting  = false;  // true = 被控端（推送），false = 控制端（接收）
};

// 一句话诊断。只回答「该往哪儿看」，不堆数字 —— 数字在 HUD 那几行里。
enum class QualityVerdict
{
    NoData,       // 还没收到过任何一帧
    Good,
    NetworkSlow,  // 往返太高
    DecodeSlow,   // 本机解码吃紧
};

// 判据门槛。公开出来是为了让用例与实现共用同一个数，而不是各写一遍
//（各写一遍的结局是「改了门槛，用例还在验旧值」，而它照样通过）。
//
// · 往返 80 ms：局域网里正常是 1–5 ms。超过 80 ms 时，一次「点下去看到反应」
//   要叠上编码 33 ms（30 fps）+ 往返 + 解码，「跟手」就开始明显变差。
// · 解码**看占了多少帧预算**（`kSlowDecodeShare`），不用固定毫秒数。
//   固定门槛的教训：原先写 12 ms，而 4K 软解（解码 + NV12→ARGB 转换）实测就在
//   12–18 ms 这一档（4K 缩到 1710×962 单格是 17.8 ms/帧），于是只要看 4K
//   就会一直报「本机解码吃紧」—— 可画面根本不卡，因为 30 fps 的预算是 33 ms。
//   同一个 15 ms 在 1080p 上是吃紧、在 4K 上是常态，所以判据只能是**比例**。
constexpr int    kSlowRttMs       = 80;
constexpr double kSlowDecodeShare = 0.5;   // 解码占掉半个帧间隔就算吃紧

QualityVerdict judgeQuality(const QualitySample &sample);

// 诊断标签（短，能塞进状态栏尾部）。
QString qualityVerdictText(QualityVerdict verdict);

// 悬停状态栏时显示的完整读数：分辨率 / 帧率 / 码率 / 往返 / 解码 / 累计 / 诊断。
//
// 被控端那一侧没有「往返」与「解码」：它的数字是发出去多少，不是收回来多少。
// 硬塞两行「—」只会让人以为读数坏了。
QString qualityHudText(const QualitySample &sample);

} // namespace lanrdp
