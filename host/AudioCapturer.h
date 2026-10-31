#pragma once

#include "protocol/AudioChunk.h"

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QThread>

#include <atomic>

namespace lanrdp {

// 被控端的系统声音采集（WASAPI loopback）：**自己的线程**，`run()` 里跑设备循环。
//
// 为什么必须是独立线程：采集是「等事件 → 取包 → 攒块」的阻塞循环，挂在宿主线程上
// 会和「抓屏 + H.264 编码 + 输入注入」抢同一条事件循环（那已经是 45 ms/帧的大头）。
//
// 三处最容易写错的地方（都在实现里有对应注释）：
//   ① 静默期**不产生事件** —— 系统不出声时 WASAPI 不给任何回调，只等事件就会
//      「一安静就再也不出声」（且之后恢复了也继续哑）；必须叠一个 50 ms 轮询。
//   ② 每个线程要自己 `CoInitializeEx` / `CoUninitialize`（COM 是线程局部的）。
//   ③ `AUDCLNT_E_DEVICE_INVALIDATED`（拔蓝牙、切默认设备）必须重建 client，
//      否则这一路从此静音。
class AudioCapturer : public QThread
{
    Q_OBJECT

public:
    explicit AudioCapturer(QObject *parent = nullptr);
    ~AudioCapturer() override;

    // 起采集。**唯一的启动入口** —— 别直接调 `start()`：停机标志必须在这条路径上
    // 清零（放在 `run()` 入口会与「刚 start 就 stop」的窗口打架，见实现注释）。
    void startCapture();

    // 请求中断并等待线程退出（`run()` 最长阻塞 50 ms ⇒ 有界）。
    // 幂等：没跑起来时直接返回。
    void stopAndWait();

    // 起不来 / 中途放弃的原因（空串 = 没有错误）。
    QString lastError() const;

    // 当前采集格式（首次成功打开后才有效）。
    AudioFormat format() const;

signals:
    // 采集格式确定或变化（换默认设备时）。
    void formatChanged(const lanrdp::AudioFormat &format);
    // 一块 PCM（长度 = `audioBlockBytes(format)`）。跨线程排队投递。
    void chunkReady(qint64 tsMs, const QByteArray &pcm, const lanrdp::AudioFormat &format);
    // 采集彻底不可用（只发一次，此后线程退出）。**不影响画面链路**。
    void failed(const QString &reason);

protected:
    void run() override;

private:
    void setError(const QString &text);

    // 自己维护停机标志，**不用** `QThread::requestInterruption()`：本对象会被
    // 「停掉再起」（重新开始监听），而「interruption 标志在下次 start() 时是否
    // 清零」是 Qt 的实现细节 —— 押它等于押一个不保证的语义。
    std::atomic<bool> stopRequested_{false};

    mutable QMutex mutex_;
    QString        lastError_;
    AudioFormat    format_;
};

} // namespace lanrdp
