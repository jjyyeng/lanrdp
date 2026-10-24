#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QSize>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <deque>
#include <memory>

namespace lanrdp {

class H264Decoder;

// 把「H.264 解码 + 色彩转换（含缩放）」搬到一个**专属工作线程**。
//
// 为什么必须这么做（压测实测，2026-09-24）：4 路 4K @10fps 挤在同一根 UI 线程上占
// **125–155%**（物理上塞不进去），单帧最坏阻塞 395 ms —— 表现是拖窗口卡、按钮点不动。
// H.264 解码本体只 8.6 ms，大头是色彩转换（28.9 ms，占 77%）；把它缩到格子量级
// 仍不够，所以只能换线程。
//
// ---------------------------------------------------------------------------
// ⚠️ 为什么是「显式队列」而不是 Qt 事件循环 + invokeMethod（踩过，记为教训）
//
// 直觉做法是继承 QThread、run() 里 exec() 起事件循环、外面用
// `QMetaObject::invokeMethod(worker, "decode", Qt::QueuedConnection, ...)` 投递。
// **这个做法不成立**：`QThread` 子类的 QObject **线程亲和性在创建它的线程**（通常是
// UI 线程），只有 run() 跑在新线程上 —— 两者不是一回事。于是那个队列事件被投递到
// **UI 线程**的事件循环里，slot 还是在 UI 线程执行，解码又回到了 UI 线程。
// 更糟的是它静默生效：画面能出来，只是照样卡。
//
// 所以这里改用**互斥量 + 条件变量 + 队列**：工作线程跑自己的循环，`decode()` /
// `setOutputSize()` 只是线程安全地入队。调用方无从选错线程 —— API 从「正确使用很难」
// 变成「错误使用不可能」。代价是这几十行队列代码，换掉一个静默失效的坑，值。
// ---------------------------------------------------------------------------
//
// ⚠️ 三条硬约束（违反会静默出错，不是崩溃就是画面全黑）：
// 1. **必须在工作线程内 `open()`** —— `MFTEnumEx` 挑的是 `MFT_ENUM_FLAG_SYNCMFT`
// 的纯软解，同步 MFT 有**线程亲和性**；且 `MfRuntime` 的 COM 初始化是线程局部的。
// 跨线程搬运一个已 open 的 decoder 是不合法的（H264Decoder 里有断言兜底）。
// 2. 工作线程结束前必须**配对 `releaseMfRuntime()`**，且 decoder 的 Release 要在
// 该线程内执行 —— `decoder_` 在 `run()` 里创建、在 `run()` 退出前析构，
// `close()` 里那句 `mft->Release()` 自然走在工作线程上。
// 3. 结果**只能走信号**回主线程（`QImage` 隐式共享，值传递安全）；**绝不**在工作
// 线程里碰 `ScreenRenderer` / 任何 QWidget。
//
// 生命周期：每条连接一个实例（不自带线程池）。
// worker->start(); // 起线程（QThread 自带）
// if (!worker->waitReady()) {...} // 等线程内 open() 的结论
// worker->decode(bitstream); // 线程安全入队，结果走 frameDecoded
// worker->stop(); // 停线程并 join（析构里兜底）
class DecoderWorker : public QThread
{
    Q_OBJECT

public:
    // `width` / `height` 只是提示值，与 `H264Decoder::open()` 同口径：
    // 解码器解析出码流头后以实际尺寸为准。
    DecoderWorker(int width, int height, QObject *parent = nullptr);
    ~DecoderWorker() override;

    DecoderWorker(const DecoderWorker &)            = delete;
    DecoderWorker &operator=(const DecoderWorker &) = delete;

    // 阻塞直到 `run()` 里 open() 有结论。返回是否成功（失败原因见 lastError()）。
    // 上层（ClientConnection）在收首帧时同步等一次 —— 开 MFT 最多几十毫秒，
    // 不值得为它把「收帧 -> 入场」做成异步状态机。
    bool waitReady(int timeoutMs = 5000);
    bool isReady() const;
    QString lastError() const;
    QString decoderName() const;

    // 解码器报出的**码流真实尺寸**（不是输出图尺寸；缩放口径另走 setOutputSize）。
    int width() const;
    int height() const;

    // 显示预算 —— 线程安全入队，在工作线程里转发给解码器。
    // **可以在 start() 之前调**（此时只记值，run() 建好解码器后补发）。
    void setOutputSize(const QSize &size);

    // 送入一帧 Annex-B 码流。线程安全入队，结果以 `frameDecoded` 信号回主线程。
    // `keyframe` 供队列超限时决定「丢到哪儿」：丢到**下一个关键帧**，参考帧链能在
    // IDR 处自行恢复。
    void decode(const QByteArray &bitstream, bool keyframe = false);

    // 停线程：置停止位 → 唤醒 → join。幂等；析构里兜底再调一次。
    // **必须**是 stop() 而不是 delete —— QThread 还在跑就被析构会直接 abort 进程。
    void stop();

signals:
    // 一帧产出（一次解码可能产出 0..N 张 —— 解码器有重排序延迟）。
    // `sourceSize` = 码流真实尺寸，与 `image.size()` 不同（后者可能已被缩放）。
    void frameDecoded(const QImage &image, const QSize &sourceSize);
    // 解码出错（坏帧 / 丢参帧很常见，不代表线程挂掉，会继续等下个关键帧）。
    void decodeFailed(const QString &message);

private:
    void run() override;

    // 跨线程可观测状态（waitReady 轮询用）。错误串不能原子读，故用 QMutex + 直白字段。
    mutable QMutex mutex_;
    QWaitCondition   wakeup_;

    bool    hasResult_ = false;  // 线程内 open() 是否已有结论
    bool    opened_    = false;  // 结论是否为成功
    QString openError_;
    QString openName_;
    int     openWidth_  = 0;
    int     openHeight_ = 0;

    // 待解码队列。**有上限**：积压意味着「解码跟不上源」，继续排只会让延迟单调
    // 增长（看到的画面越来越旧），与「保流畅优先」相反。超限时不是随便丢，而是
    // **跳到下一个关键帧** —— H.264 的参考帧链在 IDR 处能自行恢复，所以「丢到 IDR」
    // 既能砍掉积压、又不会让后续 P 帧连着解不出来。
    struct PendingFrame
    {
        QByteArray bitstream;
        bool       keyframe = false;
    };
    std::deque<PendingFrame> queue_;
    static constexpr qsizetype kMaxQueueFrames = 60;   // 约「源 60 fps 下的 1 秒」

    // 队列刚丢过帧（等下一个关键帧）时置位：此期间到达的 P 帧一律丢弃，
    // 直到一个关键帧把参考链重新建立起来。
    bool droppingUntilKeyframe_ = false;

    // 待应用的显示预算（QSize() = 无待办）
    QSize pendingOutput_;
    bool  hasPendingOutput_ = false;

    bool stopping_ = false;

    // ⚠️ 只在工作线程内创建 / 使用 / 析构
    std::unique_ptr<H264Decoder> decoder_;
    const int                    wantWidth_;
    const int                    wantHeight_;
};

} // namespace lanrdp
