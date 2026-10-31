#pragma once

#include "protocol/AudioChunk.h"

#include <QByteArray>
#include <QMutex>

#include <deque>

namespace lanrdp {

// 播放端的抖动缓冲：**只看缓冲字节深度，不读任何时钟**。
//
// 为什么不需要时钟：播放线程按设备节拍来取数据，攒太多就丢旧块、取不到就让引擎
// 自然静音 —— 这本身就是「漂移自愈」。两端没有公共时基，硬要按时间戳调度就是把
// 时钟偏差当成延迟。整链路延迟 ≈ 攒块 20 + 本缓冲 60 + 设备队列 50 ≈ **130 ms**
//（`AudioPlayer` 只按 `kTargetQueuedMs` 维持设备队列，不再写满 client 缓冲）。
//
// 线程模型：`push` 由收帧的 UI 线程调用、`take` 由播放线程调用，内部自带互斥量
//（调用方不必再包一层）。
class AudioJitterBuffer
{
public:
    struct TakeResult
    {
        QByteArray data;         // 长度 = 请求长度（按帧对齐后），不足部分补静音
        bool       underrun = false;  // 本次取不满（补过静音）
    };

    // 目标深度 60 ms（≈3 块）、上限 200 ms（≈10 块）。
    // 上限不是「能缓存多少」，而是「超过就必须让延迟降回来」的判定线。
    static constexpr int kTargetMs = 60;
    static constexpr int kMaxBufferedMs = 200;

    // 换格式（换设备）时调：重算对齐与上下限，并清空缓冲（旧格式的字节长度
    // 对新格式没有意义，留着就是噪声）。
    void configure(const AudioFormat &format);

    void push(const QByteArray &pcm);
    TakeResult take(qsizetype bytes);

    // 清缓冲 + 清统计。切格 / 关开关 / 该格断连都要调 —— 不清的话会先把
    // 上一路的尾巴（最多 200 ms）播完，听起来像「另一台电脑的声音漏进来了」。
    void clear();

    qsizetype bufferedBytes() const;
    qsizetype droppedBytes() const;
    quint64   underruns() const;
    bool      isEmpty() const;

private:
    qsizetype bytesForMs(int ms) const;

    mutable QMutex         mutex_;
    std::deque<QByteArray> blocks_;
    qsizetype              buffered_     = 0;
    qsizetype              blockAlign_   = 0;
    qsizetype              bytesPerMs_   = 0;
    qsizetype              targetBytes_  = 0;
    qsizetype              maxBytes_     = 0;
    qsizetype              droppedBytes_ = 0;
    quint64                underruns_    = 0;
};

} // namespace lanrdp
