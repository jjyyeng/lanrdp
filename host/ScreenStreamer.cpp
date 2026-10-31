#include "host/ScreenStreamer.h"

#include "host/AdaptiveBitrate.h"
#include "host/ColorConvert.h"
#include "host/ScreenCapturer.h"
#include "net/Session.h"
#include "protocol/Frame.h"
#include "protocol/H264Frame.h"
#include "protocol/ScreenFrame.h"

#include <QTimer>

#include <utility>

namespace lanrdp {

namespace {

// 一次抓帧失败后跳过多少个 tick 再试。
// 不加闸门的话，锁屏期间会按帧率频率重建 D3D 设备（枚举适配器 + CreateDevice 代价不低）。
// 默认 100 ms 间隔下 ≈ 1 s 重试一次。
constexpr int kRetryCooldownTicks = 10;

// 「非可恢复」失败连续多少次才放弃。可恢复失效（ACCESS_LOST 一类）**不看这个数**——
// 那类中断天生会自愈，而它的典型时长（锁屏）可能达数分钟。
constexpr int kMaxConsecutiveFatalFailures = 3;

} // namespace

ScreenStreamer::ScreenStreamer(QObject *parent)
    : QObject(parent)
{
    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &ScreenStreamer::onTick);
}

ScreenStreamer::~ScreenStreamer()
{
    delete capturer_;
    capturer_ = nullptr;
}

CaptureStatus ScreenStreamer::captureStatusForInitFailure(bool recoverable)
{
    return recoverable ? CaptureStatus::RecoverableLost : CaptureStatus::Failed;
}

// 判断该目标此刻能否接收本帧：积压未超硬上限即可。
// 超标则记下「首次超标时刻」；恢复即清零。持续到上限即标记待移除
// （由调用方在遍历结束后统一移除，避免在 for 循环里破坏迭代器）。
bool ScreenStreamer::targetAcceptsFrame(Session *session)
{
    if (!session)
        return false;

    const qint64 pending = pendingSource_ ? pendingSource_() : session->pendingBytes();
    if (pending <= kMaxPendingBytes) {
        congestedSinceMs_.remove(session);
        return true;
    }

    // 按**墙钟**计时，而不是「产出的帧数 × 抓帧间隔」：后者在桌面静止时会被
    // 无限推后（出帧稀疏 ⇒ 累计慢），极端情况下永不触发，卡死的接收端会一直占着会话槽。
    if (!congestionClock_.isValid())
        congestionClock_.start();
    const qint64 now = congestionClock_.elapsed();

    const auto it = congestedSinceMs_.constFind(session);
    if (it == congestedSinceMs_.constEnd()) {
        congestedSinceMs_.insert(session, now);
        return false;
    }
    if (now - it.value() >= kMaxCongestionMs) {
        congestedSinceMs_.remove(session);
        droppedTargets_.append(session);
        return false;
    }
    return false;
}

// 巡检一遍所有目标的积压状态。与「本 tick 是否产出新帧」解耦 ——
// 桌面静止时 onTick 会在拿到 NoNewFrame 后直接返回，若只在发帧处判，
// 卡死的接收端就一直不会被断开。
void ScreenStreamer::checkCongestion()
{
    if (targets_.isEmpty())
        return;
    for (Session *target : std::as_const(targets_))
        targetAcceptsFrame(target);   // 只更新状态；是否真的发帧由发帧处决定
    dropCongestedTargets();
}

// 遍历结束后统一处置被判定为「持续超标」的目标：移出并通知上层。
void ScreenStreamer::dropCongestedTargets()
{
    if (droppedTargets_.isEmpty())
        return;
    const QVector<Session *> dead = droppedTargets_;
    droppedTargets_.clear();
    for (Session *session : dead) {
        targets_.removeAll(session);
        congestedSinceMs_.remove(session);
        emit targetCongested(session,
                             QStringLiteral("发送队列积压持续超过 %1 MB（%2 ms），已停止推帧")
                                 .arg(kMaxPendingBytes / (1024 * 1024))
                                 .arg(kMaxCongestionMs));
    }
}

void ScreenStreamer::setIntervalMs(int ms)
{
    intervalMs_     = qMax(1, ms);
    baseIntervalMs_ = intervalMs_;
    adaptiveFps_    = 0;   // 基准变了，重置自适应 fps 跟踪
    if (timer_->isActive())
        timer_->start(intervalMs_);
}

void ScreenStreamer::applyIntervalMs(int ms)
{
    intervalMs_ = qMax(1, ms);
    if (timer_->isActive())
        timer_->start(intervalMs_);
}

void ScreenStreamer::setSource(FrameSource source)
{
    source_ = std::move(source);
}

void ScreenStreamer::setPendingSource(std::function<qint64()> source)
{
    pendingSource_ = std::move(source);
}

void ScreenStreamer::setCodec(StreamCodec codec)
{
    if (codec_ == codec)
        return;
    codec_ = codec;
    dropEncoder();          // 换编码方式要重建编码器（尺寸/参数都可能不同）
    h264Warned_ = false;
    codecNote_.clear();     // 编码器要重建，上一条说明随之作废
}

void ScreenStreamer::setH264Params(int fps, int bitrateBps, int gopSeconds)
{
    h264Fps_        = qMax(1, fps);
    h264Bitrate_    = qMax(100000, bitrateBps);
    h264GopSeconds_ = gopSeconds;
    dropEncoder();          // 参数变了，下次抓帧时按新参数重建
}

int ScreenStreamer::adaptiveBitrate() const
{
    return adaptiveBitrate_ > 0 ? adaptiveBitrate_ : h264Bitrate_;
}

QString ScreenStreamer::codecInfo() const
{
    return encoder_ ? encoder_->encoderName() : QString();
}

void ScreenStreamer::dropEncoder()
{
    if (encoder_) {
        encoder_->close();
        encoder_.reset();
    }
    forceKeyframe_        = true;
    initialBitrateApplied_ = false;
    adaptiveBitrate_       = 0;   // 新编码器，自适应码率回到用户值
}

void ScreenStreamer::addTarget(Session *session)
{
    if (!session || targets_.contains(session))
        return;
    targets_.append(session);
    // 新观看者的解码器是空的，必须尽快收到一个 IDR 才能出画面；
    // 否则要干等到下一个自然关键帧（可能好几秒）。
    forceKeyframe_ = true;
}

void ScreenStreamer::removeTarget(Session *session)
{
    targets_.removeAll(session);
    congestedSinceMs_.remove(session);
    droppedTargets_.removeAll(session);
}

void ScreenStreamer::clearTargets()
{
    targets_.clear();
    congestedSinceMs_.clear();
    droppedTargets_.clear();
}

void ScreenStreamer::setOutputIndex(int index)
{
    // 同一个值不去抖：用户连点同一块屏不该反复重建抓屏器 / 刷 IDR。
    if (outputIndex_ == index)
        return;

    outputIndex_ = index;

    // 作废抓屏器，下个 tick 由 captureFrame 惰性重建（与 ACCESS_LOST 自愈同一条路径，
    // 所以「换屏」这件事本身不需要第二套重建机制）。
    if (capturer_) {
        delete capturer_;
        capturer_ = nullptr;
    }

    // ⚠️ 必须显式强制 IDR。encodeH264 只在「尺寸变了」时 dropEncoder()，
    // 两块屏分辨率相同时编码器完全不动 ⇒ 不强制的话换屏后第一帧是 P 帧，
    // 控制端要花屏到下一个自然关键帧（默认 GOP 2 s）。
    forceKeyframe_ = true;

    // 上一轮的失败退避不该挡住新选中的屏 —— 切屏常常发生在「刚拔掉一块屏」之后，
    // 那时冷却计时器可能正压着，不清掉就会「切过去三秒没画面」。
    retryCooldown_            = 0;
    consecutiveFatalFailures_ = 0;
}

void ScreenStreamer::requestKeyframe()
{
    forceKeyframe_ = true;
}

void ScreenStreamer::start()
{    // 重新开始推送时不带着上一轮的退避余量。
    retryCooldown_ = 0;
    if (!timer_->isActive())
        timer_->start(intervalMs_);
}

void ScreenStreamer::stop()
{
    timer_->stop();
}

bool ScreenStreamer::isRunning() const
{
    return timer_->isActive();
}

void ScreenStreamer::resetStats()
{
    framesSent_ = 0;
    bytesSent_  = 0;
}

QString ScreenStreamer::sourceInfo() const
{
    return capturer_ ? capturer_->deviceInfo() : QString();
}

QRect ScreenStreamer::capturedRect() const
{
    if (capturer_ && capturer_->isReady())
        return capturer_->monitorRect();
    return QRect();
}

CaptureStatus ScreenStreamer::captureFrame(QImage *out, QString *error)
{
    if (error)
        error->clear();

    // 1) 注入的帧源优先（测试 / 自定义源）
    if (source_)
        return source_(out);

    // 2) 内置 DXGI 抓屏，惰性初始化
    if (!capturer_) {
        capturer_ = new ScreenCapturer();
        QString initError;
        bool    recoverable = false;
        if (!capturer_->init(&initError, &recoverable, outputIndex_)) {
            delete capturer_;
            capturer_ = nullptr;
            if (error)
                *error = initError;
            // 「创建即失败」也分两档：例如被控端**启动时**正锁屏 / 显示输出被
            // ToDesk 独占（DuplicateOutput 返 E_ACCESSDENIED）属于可恢复 —— 此时
            // 必须回 RecoverableLost 才会走「永不放弃、限频重试」那条路；若一律
            // 回 Failed，连续几次后就停流了，用户得手动重新【开始监听】。
            return captureStatusForInitFailure(recoverable);
        }

        // 请求的那块屏不在了 ⇒ ScreenCapturer 已回落主屏。把**真实** index 记下来，
        // 这样上层广播出去的 SCREEN_LIST 与实际在抓的那块永远一致
        //（否则控制端菜单会一直勾在一块不存在的屏上）。
        outputIndex_ = capturer_->outputIndex();
    }

    QString captureError;
    // 等新帧的上限。`AcquireNextFrame` 在桌面静止时会**等满**它，而这段等待发生在
    // 与 socket 读 / 输入注入**同一条线程**上 —— 所以它直接计入最坏响应延迟。
    // 原先写死 100 ms（那是「抓屏 / 编码 / socket / 注入全挤在 GUI 线程」年代为
    // 「别把界面冻太久」留的折中）。
    //
    // 改到 15 ms 的依据 —— 实测（tests/test_hostcontroller.cpp::
    // hostStaysResponsiveWhileStreaming 量的就是推流期间 `TokenReq -> TokenGrant`
    // 往返，每轮 25 个样本，本机 loopback）：
    //
    //   本轮等待上限     中位              最大
    //   100 ms          61 ms            201 ms
    //    15 ms          46–62 ms        171–266 ms
    //     1 ms          46–47 ms        139–153 ms   ← 再压已无收益
    //
    // 即：**100 → 15 有收益（中位约 −14 ms），15 → 1 没有**。所以 15 ms 是拐点，
    // 也避免了每拍空转轮询。
    //
    // ⚠️ **别指望它把延迟压到 15 ms 量级**：往返的大头**不是**这里的等待，而是同一拍里的
    // 「抓屏拷贝 + 色彩转换 + H.264 编码 + 扇出」（1 ms 那轮的中位仍是 46 ms 即可反证）。
    // 要再往下压只有一条路：把采集/编码与 socket/注入**拆成两条线程**。
    //
    // 代价为零：tick 频率由定时器决定、不因超时变短而变密；等待本身几乎不吃 CPU。
    constexpr int kMaxAcquireWaitMs = 15;
    const CaptureStatus status =
        capturer_->capture(out, qMin(intervalMs_, kMaxAcquireWaitMs), &captureError);

    if (status == CaptureStatus::Ok)
        return status;
    if (status == CaptureStatus::NoNewFrame)
        return status;  // 桌面无变化，不是错误，跳过本次发送

    // 失败的抓屏器**一律作废**，下个 tick 走上面的惰性初始化重建。
    // · 旧实现只在 `capturer_ == nullptr` 时初始化 —— 一旦初始化过，死掉的 duplication_
    // 就再也没有重建的机会，这正是「一次 ACCESS_LOST 之后画面永久冻结」的根因。
    // · ScreenCapturer::init 内部第一步就是 shutdown()，所以反复重建是安全的。
    // · RecoverableLost 时 ScreenCapturer 已经自我作废（ready_=false、monitorRect_ 清空），
    // 因此这里不需要额外处理「过期的注入几何」。
    delete capturer_;
    capturer_ = nullptr;

    if (error)
        *error = captureError;
    return status;
}

bool ScreenStreamer::encodeH264(const QImage &frame, QVector<H264EncodedFrame> *out, QString *error)
{
    if (error)
        error->clear();
    if (frame.isNull())
        return false;

    const int width  = frame.width();
    const int height = frame.height();
    if (width <= 0 || height <= 0)
        return false;
    if ((width % 2) != 0 || (height % 2) != 0) {
        if (error) {
            *error = QStringLiteral("画面尺寸 %1x%2 为奇数，NV12 不支持").arg(width).arg(height);
        }
        return false;
    }

    // 分辨率变了（例如换了显示器）就重建编码器
    if (encoder_ && (encoder_->width() != width || encoder_->height() != height))
        dropEncoder();

    // 惰性创建：第一次真正要发画面时才打开编码器
    if (!encoder_) {
        auto candidate = std::make_unique<H264Encoder>();
        if (!candidate->open(width, height, h264Fps_, h264Bitrate_, h264GopSeconds_)) {
            if (error)
                *error = candidate->lastError();
            return false;
        }
        encoder_       = std::move(candidate);
        forceKeyframe_ = true;   // 新编码器必须先出一个关键帧，解码端才能起步
    }

    const QImage source = (frame.format() == QImage::Format_ARGB32)
                              ? frame
                              : frame.convertToFormat(QImage::Format_ARGB32);
    const QByteArray nv12 = bgraToNv12(source.constBits(),
                                       static_cast<int>(source.bytesPerLine()), width, height);
    if (nv12.isEmpty()) {
        if (error)
            *error = QStringLiteral("BGRA -> NV12 转换失败");
        return false;
    }

    QVector<H264EncodedFrame> encoded;
    if (!encoder_->encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()), nv12.size(),
                               forceKeyframe_, encoded)) {
        if (error)
            *error = encoder_->lastError();
        return false;
    }
    forceKeyframe_ = false;

    // 首帧后设真实码率 —— open 阶段的码率设置无效（见 bench_adaptive_bitrate），
    // 必须等编码器开始流式处理后再 setBitrate 一次，用户设的码率才会真正生效。
    if (!initialBitrateApplied_) {
        if (encoder_->setBitrate(h264Bitrate_)) {
            initialBitrateApplied_ = true;
            adaptiveBitrate_       = h264Bitrate_;
        } else if (!encoder_->bitrateControlAvailable()) {
            // 编码器没有 ICodecAPI：码率永远设不进去。**只报一次**并停止每帧重试
            //（从前这行会一路重试到关机，且对外看不出任何异常）。
            initialBitrateApplied_ = true;
            codecNote_ = QStringLiteral("编码器未提供 ICodecAPI：码率与强制关键帧不可控"
                                        "（新观看者可能要等到下一个 GOP 才有画面）");
            emit encoderNote(codecNote_);
        }
    }

    if (out)
        *out = encoded;
    return true;
}

void ScreenStreamer::onTick()
{
    if (targets_.isEmpty())
        return;  // 没有观看者就不抓屏，省 CPU

    // 背压巡检先于抓帧：桌面静止时本 tick 可能拿不到新帧（下面直接 return），
    // 卡死的接收端不能因此逃过判定。
    checkCongestion();

    // 失败后的限频闸门（见 kRetryCooldownTicks）。
    if (retryCooldown_ > 0) {
        --retryCooldown_;
        return;
    }

    applyAdaptiveStep();

    QImage  frame;
    QString error;

    const CaptureStatus status = captureFrame(&frame, &error);

    if (status == CaptureStatus::NoNewFrame)
        return;

    // **不再「首次失败即 stop()」**。旧实现在这里 stop()，定时器一停
    // 就再也没有下一次机会 —— 被控端的画面在弹一次 UAC 之后永久冻结，只能重启程序。
    if (status != CaptureStatus::Ok) {
        handleCaptureFailure(status, error);
        return;
    }

    // 成功一帧：清掉致命失败计数；若刚从「中断」里出来，通知恢复。
    consecutiveFatalFailures_ = 0;
    if (captureInterrupted_) {
        captureInterrupted_ = false;
        emit captureResumed();
    }

    emit frameCaptured(frame);

    // ---- H.264 路径 ----
    if (codec_ == StreamCodec::H264) {
        QVector<H264EncodedFrame> encoded;
        QString                   encodeError;

        if (encodeH264(frame, &encoded, &encodeError)) {
            for (const H264EncodedFrame &unit : std::as_const(encoded)) {
                H264Frame h264;
                h264.width     = static_cast<quint32>(frame.width());
                h264.height    = static_cast<quint32>(frame.height());
                h264.keyframe  = unit.keyframe;
                h264.bitstream = unit.bitstream;

                const QByteArray payload = encodeH264Frame(h264);
                if (payload.isEmpty())
                    continue;

                int pushed = 0;
                for (Session *target : std::as_const(targets_)) {
                    if (targetAcceptsFrame(target)) {
                        target->sendFrame(static_cast<std::uint8_t>(FrameType::ScreenFrameH264),
                                          payload);
                        ++pushed;
                        bytesSent_ += static_cast<quint64>(payload.size());
                    }
                }
                dropCongestedTargets();
                if (pushed > 0)
                    ++framesSent_;
            }
            // 编码器有流水线延迟，本次没产出属正常，不能因此退回裸帧
            return;
        }

        // 编码失败不是「这一帧没产出」，而是「编码器不可用」。
        // 逐帧重试只会白烧 CPU（每帧都建一次失败的编码器），
        // 直接把 codec_ 打成裸帧：下一帧起走下方的裸帧路径，彻底不再重试。
        // 注意 codec_ 只记实际在用，「用户意图」由 setCodec 保留 —— 用户
        // 重新选 H.264 时 setCodec 会复位 h264Warned_ 并重建编码器。
        codec_ = StreamCodec::RawFrames;
        dropEncoder();

        if (!encodeError.isEmpty() && !h264Warned_) {
            h264Warned_ = true;
            emit codecFallback(encodeError);
        }
    }

    // ---- 裸帧路径 ----
    const QByteArray payload = encodeRawFrame(frame);
    if (payload.isEmpty())
        return;

    int pushed = 0;
    for (Session *target : std::as_const(targets_)) {
        if (targetAcceptsFrame(target)) {
            target->sendFrame(static_cast<std::uint8_t>(FrameType::ScreenFrameRaw), payload);
            ++pushed;
            bytesSent_ += static_cast<quint64>(payload.size());
        }
    }
    dropCongestedTargets();

    if (pushed > 0)
        ++framesSent_;
}

void ScreenStreamer::applyAdaptiveStep()
{
    // 裸帧也要参与背压 —— 它没有压缩比可调，唯一的减压手段就是降帧率。
    // H.264 路径额外要求编码器已就绪（它才有 setBitrate）。
    const bool h264 = (codec_ == StreamCodec::H264);
    if (targets_.isEmpty() || (h264 && !encoder_))
        return;

    qint64 maxPending = 0;
    if (pendingSource_) {
        maxPending = pendingSource_();
    } else {
        for (Session *target : std::as_const(targets_))
            if (target)
                maxPending = qMax(maxPending, target->pendingBytes());
    }

    const int curBitrate = adaptiveBitrate_ > 0 ? adaptiveBitrate_ : h264Bitrate_;
    const int curFps     = adaptiveFps_ > 0 ? adaptiveFps_ : qMax(1, 1000 / qMax(1, intervalMs_));

    // 边界由**用户设定**导出（见 deriveAdaptiveParams 的不变式）。
    // 旧写法 `max = qMax(500000, h264Bitrate_)` 会把用户设的低码率反向抬到 500k。
    AdaptiveParams params =
        deriveAdaptiveParams(h264Bitrate_, 1000 / qMax(1, baseIntervalMs_));
    // 裸帧没有 setBitrate：让 adaptStep 跳过降价分支、直接动帧率。
    // 同理，编码器没提供 ICodecAPI 时码率也调不动 —— 一并关掉降价分支。
    params.bitrateAdjustable = h264 && encoder_ && encoder_->bitrateControlAvailable();

    const AdaptiveState cur{curBitrate, curFps};
    const AdaptiveState next = adaptStep(maxPending, cur, params);

    if (next.bitrateBps != curBitrate) {
        // 只有编码器**真的接受**了新码率才更新跟踪值，
        // 否则内部状态会与编码器实际值悄悄漂开。
        if (encoder_ && encoder_->setBitrate(next.bitrateBps))
            adaptiveBitrate_ = next.bitrateBps;
    }
    if (next.fps != curFps) {
        adaptiveFps_ = next.fps;
        applyIntervalMs(1000 / next.fps);
    }
}

// 一次抓帧失败后的处置。
//
// 分两档，依据是「这类失败会不会自己好」：
// · RecoverableLost（ACCESS_LOST / DEVICE_REMOVED / DEVICE_RESET）—— UAC 安全桌面、
// 锁屏解锁、Ctrl+Alt+Del、分辨率变化、显示器唤醒。这些都是**日常**事件，而且
// 桌面一回来就能恢复，所以**永不放弃**，只限频重试。任何「连续 N 次就停」的上限
// 都会把老 bug 换成一个延迟版本重新引入（锁屏动辄数分钟）。
// · 其他 Failed（配置 / 资源类）—— 连续 kMaxConsecutiveFatalFailures 次仍无起色才停，
// 保留既有的 `failed` 语义（UI 仍能报错、`--expect-frame` 类自动化仍能观察到）。
//
// 两个新信号只在**跳变点**发一次，避免锁屏期间把日志刷爆。
void ScreenStreamer::handleCaptureFailure(CaptureStatus status, const QString &error)
{
    retryCooldown_ = kRetryCooldownTicks;

    if (!captureInterrupted_) {
        captureInterrupted_ = true;
        emit captureInterrupted(error);
    }

    if (status == CaptureStatus::RecoverableLost)
        return;  // 等桌面回来，继续重试

    if (++consecutiveFatalFailures_ >= kMaxConsecutiveFatalFailures) {
        consecutiveFatalFailures_ = 0;
        stop();
        emit failed(error);
    }
}

} // namespace lanrdp
