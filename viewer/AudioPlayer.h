#pragma once

#include "protocol/AudioChunk.h"
#include "viewer/AudioJitterBuffer.h"

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QThread>

#include <atomic>

namespace lanrdp {

// 一拍往设备里写多少帧 —— **纯函数，不碰 COM，无设备也能单测**。
//
// 判据是「让设备队列维持在一个浅深度」，不把空闲区写满：
//  - `padding`      = 设备队列里已经排着的帧数
//  - `bufferFrames` = client 缓冲总帧数（一拍最多能写的上限）
//  - `targetFrames` = 希望维持的队列深度
//  - 可写帧数再受「抖动缓冲里真实有多少数据」限制 —— **绝不补静音凑满**：
//    一补，队列就永久顶在上限上，听感固定滞后（这正是修过的那个 bug）。
//
// 返回 0 表示这一拍什么都不写：队列已达目标、设备还满、或没有数据。
int framesToWrite(int padding, int bufferFrames, int targetFrames,
                  qsizetype availableBytes, int blockAlign);

// 控制端的音频播放：**自己的线程** + WASAPI 共享渲染。
//
// 之所以不复用 Qt Multimedia：Qt 6 的 multimedia 走 ffmpeg 后端，一引就是几十 MB
// DLL（`dist/lanrdp/` 现在只有 7 个 Qt6 DLL，且不含 multimedia）。直接调 WASAPI
// 换来「零新增运行时依赖」，代价是本文件这三百多行。
//
// 线程模型：`push` / `reset` 由 UI 线程调（抖动缓冲内部自带互斥量），
// `run()` 里按设备节拍取数据。设备失效（拔蓝牙 / 换默认输出）重建 client。
class AudioPlayer : public QThread
{
    Q_OBJECT

public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    // 首次收到配置时调；线程已在跑且格式不同则按新格式重建（同步，几十毫秒）。
    // 返回 false = 格式非法 / 线程起不来 / **旧线程 3 秒内停不下来**（此时不会切格式：
    // 宁可继续播旧的，也不留「旧线程 + 新格式」那个错位噪声窗口）。原因见 lastError。
    bool begin(const AudioFormat &format);

    // 收帧线程调：入抖动缓冲。未 begin 时直接丢弃。
    void push(const QByteArray &pcm);

    // 清缓冲（切格 / 关开关 / 该格断连）。**不停线程** —— 停了再起要重建设备，
    // 而切格是高频操作。除了清内存，还要**真掐掉声卡里已排的数据**：
    // 只清内存的话，引擎手里那一段照样播完 —— 听感就是「切完还听到上一台的尾巴」。
    void reset();

    void stopAndWait();

    bool    isActive() const;
    QString lastError() const;

    // 诊断口径：已丢弃 / 已欠载的字节数（音频坏掉时的第一手证据）。
    qsizetype droppedBytes() const;
    quint64   underruns() const;

    // 设备队列深度（最近一拍的实测值，ms）。**延迟回归的判据**：
    // 这个值常驻在几百毫秒，就说明有人又把空闲区写满了。
    int queuedMs() const;
    // `reset()` 真正清掉设备队列的次数。为 0 = 「切格掐尾巴」没生效。
    quint64 flushCount() const;
    // **断流次数**：设备队列已排空、手上又没数据可写的次数（= 听众真的听空了）。
    // 不按拍计数 —— 对端一直静音会让按拍的计数 20 次/秒地涨，日志跟着刷屏。
    // 「网络抖」与「设备欠载」都表现成它涨，配合 `droppedBytes()` 与队列深度才分得开。
    quint64 starvedCount() const;

signals:
    // 播放彻底不可用（设备打不开 / 渲染失败 / 重建次数用尽 / 换格式时旧线程 3 秒不退）。
    // **可能多次发出，接收方按幂等处理** —— 反复换格式会反复走到那个超时。
    // ⚠️ 它**不代表线程已经退出**：超时那条路径上线程还在跑（发完仍没声音）。
    // **不影响画面链路**。
    void failed(const QString &reason);

protected:
    void run() override;

private:
    void setError(const QString &text);

    AudioJitterBuffer buffer_;

    // 自己维护停机标志，**不用** `QThread::requestInterruption()`：
    // 本对象会「停掉再起」（对端换默认设备要换采样率），而「interruption 标志在
    // 下一次 start() 时是否被清零」是 Qt 的实现细节 —— 押它等于押一个不保证的语义。
    // ⚠️ 清零只在 `begin()` 的启动路径做，**不在 `run()` 入口**：`start()` 与线程真
    // 跑到 `run()` 之间若有人 `stopAndWait()`，在 `run()` 里清零会把这次停机丢掉。
    std::atomic<bool> stopRequested_{false};

    // 由 UI 线程置位、渲染线程消费：Stop → Reset → Start 掐掉设备里已排的数据。
    // 不能在 `reset()` 里直接调 —— WASAPI 的 client 只在渲染线程的 COM 单元里用。
    std::atomic<bool> flushRequested_{false};

    // 渲染线程写、任意线程读的实测值（都是单调计数或瞬时快照，无需互斥）。
    std::atomic<int>     queuedMs_{0};
    std::atomic<quint64> flushCount_{0};
    std::atomic<quint64> starvedCount_{0};

    mutable QMutex mutex_;
    QString        lastError_;
    AudioFormat    format_;
    bool           hasFormat_ = false;
};

} // namespace lanrdp
