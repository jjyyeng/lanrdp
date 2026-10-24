#pragma once

#include "host/H264Encoder.h"
#include "host/ScreenCapturer.h"   // CaptureStatus（抓帧结果）

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>
#include <QVector>

#include <QHash>

#include <functional>
#include <memory>

class QTimer;

namespace lanrdp {

class Session;
class ScreenCapturer;

// 画面编码方式
enum class StreamCodec
{
    RawFrames,   // 裸 BGRA（SCREEN_FRAME_RAW）
    H264,        // Media Foundation H.264（SCREEN_FRAME_H264）
};

// 抓屏 -> 编码 -> 扇出到多条 Session 的定时推送器。
//
// 编码方式由 setCodec() 选择；H.264 走不通时（编码器打不开）自动退回裸帧，
// 保证画面不会因为压缩器不可用而整个断掉。
class ScreenStreamer : public QObject
{
    Q_OBJECT

public:
    // 帧源：返回 Ok 表示拿到新帧；未设置时使用内置 DXGI 抓屏。
    // 注入式设计让单元测试无需真实桌面即可验证分发链路。
    // 返回类型由 bool 升级为 CaptureStatus —— 测试（和内置抓屏）必须能区分
    // 「桌面无变化」「可恢复失效」「其他失败」三种情况，否则自愈路径无法被断言。
    using FrameSource = std::function<CaptureStatus(QImage *)>;

    explicit ScreenStreamer(QObject *parent = nullptr);
    ~ScreenStreamer() override;

    // 抓屏器「创建即失败」时该报哪种状态。抽成静态纯函数是为了能直接单测
    // （真实 DXGI 初始化在无桌面会话的测试环境里根本走不到那一步）。
    // recoverable=true ⇒ RecoverableLost（限频重试、不停流）；否则 Failed。
    static CaptureStatus captureStatusForInitFailure(bool recoverable);

    void setIntervalMs(int ms);
    int  intervalMs() const { return intervalMs_; }

    void setSource(FrameSource source);

    // 自适应码率：测试 / 外部可注入「发送队列积压」来源（默认空 =
    // 读真实 targets 的 socket pendingBytes）。返回积压字节数。测试用它模拟链路
    // 卡顿，无需真的慢 socket。
    void setPendingSource(std::function<qint64()> source);

    // 编码方式。切到 H264 后编码器在首帧时惰性创建。
    void        setCodec(StreamCodec codec);
    StreamCodec codec() const { return codec_; }

    // H.264 参数（码率单位 bit/s；gopSeconds<=0 表示用编码器默认值）
    void setH264Params(int fps, int bitrateBps, int gopSeconds);

    // 当前自适应后的码率（首帧后 = 用户设的码率；之后随积压升降）。
    int adaptiveBitrate() const;

    // 实际正在使用的编码器名；未启用 H.264 或初始化失败时为空串。
    QString codecInfo() const;

    // 编码器有一条「要让人看见」的说明（目前唯一来源 = 编码器没提供 ICodecAPI，
    // 于是码率与强制关键帧都下发不了）。空串 = 没有。首次产生时会发 codecNote() 信号。
    QString codecNote() const { return codecNote_; }

    // 是否真的在用 H.264 编码（开启后编码器创建成功才为 true）
    bool h264Active() const { return encoder_ != nullptr; }

    // **实际**在用的编码。H.264 一旦编码失败就永久降级为裸帧，
    // 所以这里可能比 codec() 更保守。
    StreamCodec effectiveCodec() const
    {
        return encoder_ ? StreamCodec::H264 : StreamCodec::RawFrames;
    }

    void addTarget(Session *session);
    void removeTarget(Session *session);
    void clearTargets();
    int  targetCount() const { return targets_.size(); }

    void start();
    void stop();
    bool isRunning() const;

    quint64 framesSent() const { return framesSent_; }
    quint64 bytesSent() const { return bytesSent_; }
    void    resetStats();

    // 抓屏源描述（内置 DXGI 抓屏初始化成功后才有效）
    QString sourceInfo() const;

    // 被抓显示器在屏幕坐标系中的矩形；尚未抓到首帧时返回空矩形。
    // 注入端用它把控制端的归一化坐标还原到正确的显示器上。
    QRect capturedRect() const;

signals:
    void frameCaptured(const QImage &frame);  // 被控端本地预览用
    void failed(const QString &error);
    void codecFallback(const QString &reason);  // H.264 不可用，已退回裸帧
    // 编码器能力受限的非致命说明（如没有 ICodecAPI）。只在首次产生时发一次。
    void encoderNote(const QString &note);

    // 某个目标积压持续超过硬上限，已停止向它推帧（上层可据此决定断开连接）。
    // 每目标独立触发，不影响其他目标。
    void targetCongested(lanrdp::Session *session, const QString &reason);

    // 抓屏**暂时**中断 / 已自行恢复。只在状态跳变点发一次（不刷屏）。
    // 与 failed 的区别：failed = 已放弃并停流；这两个 = 流还在，正在重试 / 已自愈。
    void captureInterrupted(const QString &reason);
    void captureResumed();

private slots:
    void onTick();

private:
    CaptureStatus captureFrame(QImage *out, QString *error);
    void dropEncoder();

    // 一次抓帧失败后的处置（限频重试 / 可恢复失效永不放弃 / 其他失败连续 N 次才停）
    void handleCaptureFailure(CaptureStatus status, const QString &error);

    // 每 tick 的积压自适应（调码率 / 间隔）
    void applyAdaptiveStep();
    // 内部改抓帧间隔（不更新基准间隔）
    void applyIntervalMs(int ms);

    // 该目标此刻是否允许推帧（积压未超硬上限）。超标时记下「首次超标时刻」，
    // 持续到上限即记入待移除列表。
    bool targetAcceptsFrame(Session *session);
    // 巡检所有目标的积压状态并移除「持续超标」的。**与是否产出新帧无关** ——
    // 桌面静止时可能连续多 tick 拿不到新帧，只在发帧处判会让卡死的目标长期不被发现。
    void checkCongestion();
    // 遍历结束后统一移除「持续超标」的目标并发 targetCongested
    void dropCongestedTargets();

    // 把一帧 BGRA 编成 H.264（内部完成 BGRA->NV12）；编码器有延迟，可能没有产出。
    bool encodeH264(const QImage &frame, QVector<H264EncodedFrame> *out, QString *error);

    QTimer            *timer_      = nullptr;
    ScreenCapturer    *capturer_   = nullptr;  // 惰性创建（首次真正需要时）
    FrameSource        source_;
    QVector<Session *> targets_;
    int                intervalMs_ = 100;
    quint64            framesSent_ = 0;
    quint64            bytesSent_  = 0;

    StreamCodec                  codec_          = StreamCodec::RawFrames;
    int                          h264Fps_        = 15;
    int                          h264Bitrate_    = 8 * 1000 * 1000;
    int                          h264GopSeconds_ = 2;
    std::unique_ptr<H264Encoder> encoder_;
    bool                         forceKeyframe_  = true;
    bool                         h264Warned_     = false;
    QString                      codecNote_;      // 编码器能力受限说明（见 codecNote()）

    // 自适应码率状态
    std::function<qint64()> pendingSource_;         // 积压来源（空 = 读真实 targets）
    int  adaptiveBitrate_ = 0;                       // 当前自适应码率（0 = 未开始，用 h264Bitrate_）
    int  adaptiveFps_     = 0;                       // 当前自适应 fps（0 = 未开始，由 intervalMs_ 派生）
    int  baseIntervalMs_  = 100;                     // 基准抓帧间隔（fps 恢复目标）
    bool initialBitrateApplied_ = false;             // 首帧后是否已 setBitrate 真实码率

    // 抓屏失败后的重试状态
    int  retryCooldown_         = 0;    // >0 时本 tick 直接跳过（限频，避免锁屏期间高频重建 D3D）
    int  consecutiveFatalFailures_ = 0; // 连续「非可恢复」失败次数（成功即归零）
    bool captureInterrupted_    = false; // 已发过 captureInterrupted（只在跳变点发，防刷屏）

    // 发送侧背压：每个目标「积压首次超上限」的时刻（毫秒，取自 congestionClock_）。
    // 达标即移出 targets_。键是 Session 指针；已断开的目标在 removeTarget/clearTargets 里清掉。
    QHash<Session *, qint64> congestedSinceMs_;
    // 单调时钟，只用于背压计时（不受系统时间调整影响）。
    QElapsedTimer         congestionClock_;
    // 本 tick 内被判定为「持续超标」的目标，遍历结束后统一移除。
    QVector<Session *>    droppedTargets_;

    // 积压硬上限。超过即跳过本帧（4K 裸帧一帧 ≈ 33 MB，8 MB 仍小于一帧，
    // 故慢端基本每帧被跳过、快端完全不受影响）。
    static constexpr qint64 kMaxPendingBytes = 8 * 1024 * 1024;
    // 持续超标多久后放弃该目标（毫秒，**墙钟**，与抓帧间隔和出帧率无关）。
    static constexpr int kMaxCongestionMs = 5000;
};

} // namespace lanrdp
