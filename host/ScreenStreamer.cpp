#include "host/ScreenStreamer.h"

#include "host/Nv12Converter.h"

#include "host/AdaptiveBitrate.h"
#include "common/ColorConvert.h"
#include "host/ScreenCapturer.h"
#include "net/Session.h"
#include "protocol/Frame.h"
#include "protocol/H264Frame.h"
#include "protocol/ScreenFrame.h"

#include <QTimer>

#include <algorithm>
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

    // 性能打点：环境变量开关，默认关。放在构造里而不是 start() 里，
    // 是为了让它与「谁创建了这个对象」无关（测试注入的帧源同样能打点）。
    profiling_ = qEnvironmentVariableIntValue("LANRDP_PERF") > 0;
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

void ScreenStreamer::setPreviewImageRequired(bool on)
{
    if (previewImageRequired_ == on)
        return;
    previewImageRequired_ = on;
    // 切回 QImage 路径时，编码器不必重建（尺寸没变）；但打点里的 path 标记
    // 会在下一拍自动更新（见 onTick）。
}

void ScreenStreamer::setGpuConversionEnabled(bool on)
{
    if (gpuConversionEnabled_ == on)
        return;
    gpuConversionEnabled_ = on;
    if (capturer_)
        capturer_->setGpuConversionEnabled(on);
    // 换了路径，「上次的结论」就过期了，让下一拍重新报一次 —— 否则日志会一直停在
    // 上一句「显卡色彩转换已启用」上，而实际上早就换成 CPU 了。
    capturePathNoted_ = false;
}


void ScreenStreamer::setPaused(bool paused)
{
    if (paused_ == paused)
        return;

    paused_ = paused;

    // 只处理「恢复」这一侧：暂停不需要做任何事（下一次 tick 自己就不干活了），
    // 而恢复必须强制 IDR —— 停发帧期间编码器的时间轴还在走，继续发 P 帧
    // 会让控制端拿着过期参考帧解不出画面。
    if (!paused_)
        dropEncoder();
}

void ScreenStreamer::resetStats()
{
    framesSent_ = 0;
    bytesSent_  = 0;
}

void ScreenStreamer::setProfilingEnabled(bool on)
{
    if (profiling_ == on)
        return;
    profiling_ = on;
    // 关掉时清干净：计时器留着（isValid 决定首拍 period 报不报），样本必须清，
    // 否则「关一阵再开」会把两段不同工况的样本混在同一行里。
    profileSamples_.clear();
    tickProfile_        = TickProfile{};
    lastInjectedRef_    = injectedCounter_ ? injectedCounter_() : 0;
    if (on)
        tickClock_.invalidate();   // 首拍没有「上一拍」，period 报 0
}

void ScreenStreamer::setInjectedCounter(std::function<quint64()> counter)
{
    injectedCounter_ = std::move(counter);
    lastInjectedRef_ = injectedCounter_ ? injectedCounter_() : 0;
}

// 一拍收尾：攒够 kProfileSamples 后汇总一行（p50 / p95），然后清空样本重新攒。
//
// 为什么必须给 p95：卡顿感来自尖峰而不是中位数。4K 软编实测方差很大，
// 出现「中位 22 ms、p95 38 ms」时，20 fps（50 ms 预算）看起来够用，
// 实际上每四五个拍就有一次踩线 —— 那才是「偶尔一顿」的来源。
void ScreenStreamer::profileTickSnapshot()
{
    if (!profiling_)
        return;

    // 注入事件增量：两次采样之间的到达数，摊到每拍。
    if (injectedCounter_) {
        const quint64 now = injectedCounter_();
        tickProfile_.injected = static_cast<qint64>(now - lastInjectedRef_);
        lastInjectedRef_      = now;
    }

    profileSamples_.append(tickProfile_);
    tickProfile_ = TickProfile{};
    if (profileSamples_.size() < kProfileSamples)
        return;

    // 分位数取「最近秩」：idx = round(q * (n-1))。n=25 时 p50 取第 12 个、p95 取第 23 个。
    // 用取值函数而不是成员指针：totalUs() 是成员**函数**，取不到成员指针。
    using FieldGetter = std::function<qint64(const TickProfile &)>;
    const auto percentile = [this](const FieldGetter &getter, double q) -> qint64 {
        QVector<qint64> values;
        values.reserve(profileSamples_.size());
        for (const TickProfile &sample : std::as_const(profileSamples_))
            values.append(getter(sample));
        std::sort(values.begin(), values.end());
        const int last = values.size() - 1;
        const int idx  = qBound(0, static_cast<int>(q * last + 0.5), last);
        return values.at(idx);
    };

    qint64 periodSum = 0;
    qint64 injectSum = 0;
    for (const TickProfile &sample : std::as_const(profileSamples_)) {
        periodSum += sample.periodUs;
        injectSum += sample.injected;
    }
    const double n = static_cast<double>(profileSamples_.size());

    const auto toMs = [](qint64 us) { return QString::number(us / 1000.0, 'f', 1); };

    QString line = QStringLiteral("perf n=%1 period=%2ms inject=%3/tick path=%4 | ")
                       .arg(profileSamples_.size())
                       .arg(toMs(periodSum / static_cast<qint64>(n)))
                       .arg(QString::number(injectSum / n, 'f', 1))
                       .arg(perfPath_.isEmpty() ? QStringLiteral("?") : perfPath_);

    // capture / convert / encode / send 分开报 —— 「谁最贵」正是这次打点唯一要回答的问题。
    line += QStringLiteral("p50 capture=%1 convert=%2 encode=%3 send=%4 total=%5")
                .arg(toMs(percentile([](const TickProfile &s) { return s.captureUs; }, 0.5)),
                     toMs(percentile([](const TickProfile &s) { return s.convertUs; }, 0.5)),
                     toMs(percentile([](const TickProfile &s) { return s.encodeUs; }, 0.5)),
                     toMs(percentile([](const TickProfile &s) { return s.sendUs; }, 0.5)),
                     toMs(percentile([](const TickProfile &s) { return s.totalUs(); }, 0.5)));
    line += QStringLiteral(" | p95 capture=%1 convert=%2 encode=%3 send=%4 total=%5")
                .arg(toMs(percentile([](const TickProfile &s) { return s.captureUs; }, 0.95)),
                     toMs(percentile([](const TickProfile &s) { return s.convertUs; }, 0.95)),
                     toMs(percentile([](const TickProfile &s) { return s.encodeUs; }, 0.95)),
                     toMs(percentile([](const TickProfile &s) { return s.sendUs; }, 0.95)),
                     toMs(percentile([](const TickProfile &s) { return s.totalUs(); }, 0.95)));

    profileSamples_.clear();
    emit perfSample(line);
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

namespace {
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
// 「抓屏拷贝 + 色彩转换 + H.264 编码 + 扇出」。要再往下压只有两条路：把每帧成本降下来
// （已做：NV12 由显卡直出，4K 单拍 40 ms → 约 25 ms），或者把采集/编码与
// socket/注入拆成两条线程（尚未做）。
//
// 代价为零：tick 频率由定时器决定、不因超时变短而变密；等待本身几乎不吃 CPU。
constexpr int kMaxAcquireWaitMs = 15;
} // namespace

bool ScreenStreamer::ensureCapturer(CaptureStatus *status, QString *error)
{
    if (capturer_)
        return true;

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
        if (status)
            *status = captureStatusForInitFailure(recoverable);
        return false;
    }

    // 请求的那块屏不在了 ⇒ ScreenCapturer 已回落主屏。把**真实** index 记下来，
    // 这样上层广播出去的 SCREEN_LIST 与实际在抓的那块永远一致
    //（否则控制端菜单会一直勾在一块不存在的屏上）。
    outputIndex_      = capturer_->outputIndex();
    capturer_->setGpuConversionEnabled(gpuConversionEnabled_);
    capturePathNoted_ = false;
    return true;
}

CaptureStatus ScreenStreamer::dropCapturerAndReport(CaptureStatus status,
                                                    const QString &captureError, QString *error)
{
    // 失败的抓屏器**一律作废**，下个 tick 走上面的惰性初始化重建。
    // · 旧实现只在 `capturer_ == nullptr` 时初始化 —— 一旦初始化过，死掉的 duplication_
    //   就再也没有重建的机会，这正是「一次 ACCESS_LOST 之后画面永久冻结」的根因。
    // · ScreenCapturer::init 内部第一步就是 shutdown()，所以反复重建是安全的。
    // · RecoverableLost 时 ScreenCapturer 已经自我作废（ready_=false、monitorRect_ 清空），
    //   因此这里不需要额外处理「过期的注入几何」。
    delete capturer_;
    capturer_         = nullptr;
    capturePathNoted_ = false;

    if (error)
        *error = captureError;
    return status;
}

CaptureStatus ScreenStreamer::captureFrame(QImage *out, QString *error)
{
    if (error)
        error->clear();

    // 1) 注入的帧源优先（测试 / 自定义源）
    if (source_)
        return source_(out);

    // 2) 内置 DXGI 抓屏，惰性初始化
    CaptureStatus initStatus = CaptureStatus::Failed;
    if (!ensureCapturer(&initStatus, error))
        return initStatus;

    QString captureError;
    const CaptureStatus status =
        capturer_->capture(out, qMin(intervalMs_, kMaxAcquireWaitMs), &captureError);

    if (status == CaptureStatus::Ok || status == CaptureStatus::NoNewFrame)
        return status;   // 桌面无变化不是错误，跳过本次发送

    return dropCapturerAndReport(status, captureError, error);
}

CaptureStatus ScreenStreamer::captureFrameNv12(QByteArray *out, int *width, int *height,
                                               QString *error)
{
    if (error)
        error->clear();
    if (!out || !width || !height)
        return CaptureStatus::Failed;

    // 1) 注入的帧源（测试 / 自定义源）：它只能给出 QImage，按老路转一次。
    //    这条路径**不碰显卡**，所以测试不需要真实桌面也不需要显卡也能跑。
    if (source_) {
        QImage frame;
        const CaptureStatus status = source_(&frame);
        if (status != CaptureStatus::Ok)
            return status;
        if (frame.isNull())
            return CaptureStatus::Failed;

        const QImage source = (frame.format() == QImage::Format_ARGB32)
                                  ? frame
                                  : frame.convertToFormat(QImage::Format_ARGB32);
        QByteArray nv12;
        if (source.isNull()
            || !Nv12Converter::convertCpu(source.constBits(), static_cast<int>(source.bytesPerLine()),
                                          source.width(), source.height(), &nv12)) {
            if (error)
                *error = QStringLiteral("BGRA -> NV12 转换失败");
            return CaptureStatus::Failed;
        }
        *width  = source.width();
        *height = source.height();
        *out    = std::move(nv12);
        return CaptureStatus::Ok;
    }

    // 2) 内置 DXGI 抓屏
    CaptureStatus initStatus = CaptureStatus::Failed;
    if (!ensureCapturer(&initStatus, error))
        return initStatus;

    QString captureError;
    const CaptureStatus status =
        capturer_->captureNv12(out, qMin(intervalMs_, kMaxAcquireWaitMs), &captureError);

    if (status == CaptureStatus::Ok) {
        *width  = capturer_->width();
        *height = capturer_->height();
        return status;
    }
    if (status == CaptureStatus::NoNewFrame)
        return status;

    return dropCapturerAndReport(status, captureError, error);
}

bool ScreenStreamer::encodeH264Nv12(const QByteArray &nv12, int width, int height,
                                    QVector<H264EncodedFrame> *out, QString *error)
{
    if (error)
        error->clear();
    if (nv12.isEmpty() || width <= 0 || height <= 0)
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

    QVector<H264EncodedFrame> encoded;
    if (profiling_)
        codecClock_.restart();
    const bool encodedOk = encoder_->encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()),
                                                 nv12.size(), forceKeyframe_, encoded);
    if (profiling_)
        tickProfile_.encodeUs = codecClock_.nsecsElapsed() / 1000;
    if (!encodedOk) {
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

bool ScreenStreamer::encodeH264(const QImage &frame, QVector<H264EncodedFrame> *out,
                                QString *error)
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

    const QImage source = (frame.format() == QImage::Format_ARGB32)
                              ? frame
                              : frame.convertToFormat(QImage::Format_ARGB32);
    // 打点：转换段与编码段必须**分开**量。合成一个数就只能得到「31 ms」，
    // 而「31 ms 里转换 10、编码 21」和「转换 25、编码 6」推导出的是完全不同的两个方案
    //（前者值得上硬编，后者值得做 GPU NV12）。
    //
    // ⚠️ 这条路径现在只在**预览开着**或**自定义帧源**（测试）时才会走到；
    // 真实推流走 captureFrameNv12 + encodeH264Nv12，转换成本记在 capture 段里。
    if (profiling_)
        codecClock_.restart();
    const QByteArray nv12 = bgraToNv12(source.constBits(),
                                       static_cast<int>(source.bytesPerLine()), width, height);
    if (profiling_)
        tickProfile_.convertUs = codecClock_.nsecsElapsed() / 1000;
    if (nv12.isEmpty()) {
        if (error)
            *error = QStringLiteral("BGRA -> NV12 转换失败");
        return false;
    }

    return encodeH264Nv12(nv12, width, height, out, error);
}

void ScreenStreamer::sendH264Units(const QVector<H264EncodedFrame> &units, int width, int height)
{
    for (const H264EncodedFrame &unit : std::as_const(units)) {
        H264Frame h264;
        h264.width     = static_cast<quint32>(width);
        h264.height    = static_cast<quint32>(height);
        h264.keyframe  = unit.keyframe;
        h264.bitstream = unit.bitstream;

        const QByteArray payload = encodeH264Frame(h264);
        if (payload.isEmpty())
            continue;

        int pushed = 0;
        for (Session *target : std::as_const(targets_)) {
            if (targetAcceptsFrame(target)) {
                target->sendFrame(static_cast<std::uint8_t>(FrameType::ScreenFrameH264), payload);
                ++pushed;
                bytesSent_ += static_cast<quint64>(payload.size());
            }
        }
        dropCongestedTargets();
        if (pushed > 0)
            ++framesSent_;
    }
}

void ScreenStreamer::onTick()
{
    // 性能打点：拍间隔在**所有早退路径之前**量 —— 「tick 有没有被别的活拖后」
    // 本身就是判据之一（文件传输 / 音频 / 大量注入都挤在同一线程）。
    // 只有真正抓到帧的拍才计入样本（见 profileTickSnapshot），否则静止桌面的
    // 15 ms 抓帧等待会把统计稀释得看不出问题。
    if (profiling_) {
        tickProfile_          = TickProfile{};
        tickProfile_.periodUs = tickClock_.isValid() ? tickClock_.nsecsElapsed() / 1000 : 0;
        tickClock_.restart();
    }

    if (targets_.isEmpty())
        return;  // 没有观看者就不抓屏，省 CPU

    // 隐私遮挡：一帧都不抓、一帧都不发。放在背压巡检**之前** —— 暂停期间
    // 没有发帧，积压只会往外排，这时候去判「谁太慢」纯属无中生有。
    if (paused_)
        return;

    // 背压巡检先于抓帧：桌面静止时本 tick 可能拿不到新帧（下面直接 return），
    // 卡死的接收端不能因此逃过判定。
    checkCongestion();

    // 失败后的限频闸门（见 kRetryCooldownTicks）。
    if (retryCooldown_ > 0) {
        --retryCooldown_;
        return;
    }

    applyAdaptiveStep();

    // ---- 走哪条抓屏路 ----
    // H.264 且没人要 QImage ⇒ **NV12 直出**：显卡把 BGRA 直接转成 NV12，
    // 省掉「填一张 4K QImage（33 MB 拷贝）+ 再读一遍做色彩转换」这整段。
    // 4K 实测：25 ms → 10.7 ms。
    // 需要 QImage 的两种情况仍走老路：本机预览（frameCaptured 就是它）、裸帧路径。
    const bool h264Path  = (codec_ == StreamCodec::H264);
    const bool needImage = previewImageRequired_ || !h264Path;

    QImage     frame;
    QByteArray nv12;
    int        nv12Width  = 0;
    int        nv12Height = 0;
    QString    error;

    if (profiling_)
        stageClock_.restart();
    const CaptureStatus status =
        needImage ? captureFrame(&frame, &error)
                  : captureFrameNv12(&nv12, &nv12Width, &nv12Height, &error);
    if (profiling_) {
        tickProfile_.captureUs = stageClock_.nsecsElapsed() / 1000;
        perfPath_ = needImage ? QStringLiteral("QImage") : QStringLiteral("NV12");
    }

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

    if (needImage) {
        emit frameCaptured(frame);
    } else {
        // 快速路径没有 QImage：尺寸得单独报，否则状态栏的分辨率读数会一直是空的。
        emit frameSizeChanged(QSize(nv12Width, nv12Height));
        // 「用上显卡没有 / 为什么没用」只报一次。显卡路径可能因驱动静默降级，
        // 而用户唯一的疑问正是「为什么没变快」。
        if (!capturePathNoted_) {
            capturePathNoted_ = true;
            const QString note = capturer_ ? capturer_->nv12PathInfo() : QString();
            if (!note.isEmpty())
                emit captureNote(note);
        }
    }

    // ---- H.264 路径 ----
    if (h264Path) {
        QVector<H264EncodedFrame> encoded;
        QString                   encodeError;

        const int frameWidth  = needImage ? frame.width() : nv12Width;
        const int frameHeight = needImage ? frame.height() : nv12Height;

        const bool encodedOk =
            needImage ? encodeH264(frame, &encoded, &encodeError)
                      : encodeH264Nv12(nv12, nv12Width, nv12Height, &encoded, &encodeError);

        if (encodedOk) {
            if (profiling_)
                stageClock_.restart();
            sendH264Units(encoded, frameWidth, frameHeight);
            if (profiling_)
                tickProfile_.sendUs = stageClock_.nsecsElapsed() / 1000;
            profileTickSnapshot();
            // 编码器有流水线延迟，本次没产出属正常，不能因此退回裸帧
            return;
        }

        // 编码失败不是「这一帧没产出」，而是「编码器不可用」。
        // 逐帧重试只会白烧 CPU（每帧都建一次失败的编码器），
        // 直接把 codec_ 打成裸帧：下一帧起走下方的裸帧路径，彻底不再重试。
        // 注意 codec_ 只记实际在用，「用户意图」由 setCodec 保留 —— 用户
        // 重新选 H.264 时 setCodec 会复位 h264Warned_ 并重建编码器。
        //
        // ⚠️ 快速路径手上只有 NV12，补不出裸帧载荷（裸帧是 BGRA）⇒ 这一帧丢掉。
        //    下一帧 needImage 就变 true，自然回到 QImage + 裸帧路径。
        codec_ = StreamCodec::RawFrames;
        dropEncoder();

        if (!encodeError.isEmpty() && !h264Warned_) {
            h264Warned_ = true;
            emit codecFallback(encodeError);
        }
        if (!needImage) {
            profileTickSnapshot();
            return;
        }
    }

    // ---- 裸帧路径 ----
    if (profiling_)
        stageClock_.restart();
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
    if (profiling_)
        tickProfile_.sendUs = stageClock_.nsecsElapsed() / 1000;

    if (pushed > 0)
        ++framesSent_;

    profileTickSnapshot();
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
