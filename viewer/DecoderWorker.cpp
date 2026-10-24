#include "viewer/DecoderWorker.h"

#include "host/MfRuntime.h"
#include "viewer/H264Decoder.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QVector>

#include <algorithm>
#include <iterator>

namespace lanrdp {

DecoderWorker::DecoderWorker(int width, int height, QObject *parent)
    : QThread(parent), wantWidth_(width), wantHeight_(height)
{
    // 线程名便于调试器里认人（同名多实例时靠 id 区分）
    setObjectName(QStringLiteral("DecoderWorker"));
}

DecoderWorker::~DecoderWorker()
{
    // 兜底：上层忘了 stop() 也不能让 QThread 在「仍在跑」的状态下被析构
    // —— 那会直接 abort 进程（"QThread: Destroyed while thread is still running"）。
    stop();
}

// ---------------------------------------------------------------------------
// 工作线程主体
// ---------------------------------------------------------------------------

void DecoderWorker::run()
{
    // 结论回填（成功 / 失败都走这里）。只在持锁段内调用。
    const auto report = [this](bool ok, const QString &err, const QString &name, int w, int h) {
        QMutexLocker lock(&mutex_);
        hasResult_  = true;
        opened_     = ok;
        openError_  = err;
        openName_   = name;
        openWidth_  = w;
        openHeight_ = h;
    };

    // ⚠️ 这里**必须**自己初始化 COM：主线程那次 ensureMfRuntime() 是线程局部的，
    // 覆盖不到本线程。同步 MFT 又要求 open() 与后续 ProcessInput/ProcessOutput 同线程。
    if (!ensureMfRuntime()) {
        report(false, QStringLiteral("MFStartup 失败"), QString(), 0, 0);
        return;
    }

    // 在本线程内创建并 open()。unique_ptr 保证下面无论从哪里返回，析构都发生在这里
    // —— close() 里的 mft->Release() 才走在正确的线程上。
    decoder_ = std::make_unique<H264Decoder>();
    if (!decoder_->open(wantWidth_, wantHeight_)) {
        const QString err = decoder_->lastError();
        decoder_.reset();
        releaseMfRuntime();
        report(false, err, QString(), 0, 0);
        return;
    }

    {
        // 建好就补发一次显示预算 —— 上层（ClientConnection）可能在我们 open 之前
        // 就调过 setOutputSize（窗口已显示 / 曾 resize）。
        QMutexLocker lock(&mutex_);
        if (hasPendingOutput_) {
            if (pendingOutput_.isValid())
                decoder_->setOutputSize(pendingOutput_);
            pendingOutput_    = QSize();
            hasPendingOutput_ = false;
        }
        hasResult_  = true;
        opened_     = true;
        openError_.clear();
        openName_   = decoder_->decoderName();
        openWidth_  = decoder_->width();
        openHeight_ = decoder_->height();
    }

    // ---- 显式队列循环 ----
    // 不用 exec()：见头文件里「为什么是显式队列」那段 —— QThread 子类的对象亲和性
    // 在创建它的线程，队列事件会投到 UI 线程而不是这里，解码会静默回到 UI 线程。
    for (;;) {
        QByteArray frame;
        QSize      newOutput;
        bool       haveFrame = false;
        bool       applyOut  = false;

        {
            QMutexLocker lock(&mutex_);
            while (!stopping_) {
                if (hasPendingOutput_) {
                    newOutput         = pendingOutput_;
                    pendingOutput_    = QSize();
                    hasPendingOutput_ = false;
                    applyOut          = true;
                    break;
                }
                if (!queue_.empty()) {
                    frame     = queue_.front().bitstream;
                    queue_.pop_front();
                    haveFrame = true;
                    break;
                }
                // 带超时地等：即便 stop() 的唤醒理论上丢了，也能靠它兜底
                wakeup_.wait(&mutex_, 50);
            }
            if (stopping_)
                break;
        }

        if (applyOut) {
            decoder_->setOutputSize(newOutput);
            continue;
        }
        if (!haveFrame)
            continue;

        QVector<QImage> images;
        if (!decoder_->decodeFrame(frame, images)) {
            // 坏帧 / 丢参帧很常见，不代表线程挂掉 —— 记录后继续等下个关键帧。
            emit decodeFailed(decoder_->lastError());
            continue;
        }

        // 真尺寸取解码器报的码流尺寸 —— 缩放在解码器内部做掉了
        // （见 setOutputSize），而 width()/height() 刻意**不跟着缩**，正好当
        // 「原图尺寸」这个口径用。状态栏分辨率、归一化映射都依赖它。
        const QSize sourceSize(decoder_->width(), decoder_->height());
        for (const QImage &image : images) {
            // 从**本线程**发射；接收方在 UI 线程 ⇒ 自动队列投递。
            // QImage 隐式共享，跨线程按值传递安全（只在真被改写时才深拷贝）。
            emit frameDecoded(image, sourceSize);
        }
    }

    // ---- 收尾（全部在本线程内）----
    // 顺序要点：先把 MFT 的流关掉再析构，最后才撤销本线程的 COM。
    decoder_->close();
    decoder_.reset();
    releaseMfRuntime();
}

// ---------------------------------------------------------------------------
// 主线程侧：启动与查询
// ---------------------------------------------------------------------------

bool DecoderWorker::waitReady(int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    for (;;) {
        {
            QMutexLocker lock(&mutex_);
            if (hasResult_)
                return opened_;
        }
        if (t.elapsed() >= timeoutMs) {
            // 只**报告**超时，绝不改写结果状态 —— 线程此刻可能正在回填真实结论
            // （开 MFT 慢了半拍而已）。在这里把 hasResult_ / opened_ 钉成「失败」，
            // 一旦这次写入落在真实结论之后，解码器就被永久标注成「没就绪」：明明
            // 在正常解码，上层却每帧都判失败、反复推倒重建。
            QMutexLocker lock(&mutex_);
            if (!hasResult_ && openError_.isEmpty())
                openError_ = QStringLiteral("解码线程启动超时");
            return false;
        }
        QThread::msleep(2);
    }
}

bool DecoderWorker::isReady() const
{
    QMutexLocker lock(&mutex_);
    return hasResult_ && opened_;
}

QString DecoderWorker::lastError() const
{
    QMutexLocker lock(&mutex_);
    return openError_;
}

QString DecoderWorker::decoderName() const
{
    QMutexLocker lock(&mutex_);
    return openName_;
}

int DecoderWorker::width() const
{
    QMutexLocker lock(&mutex_);
    return openWidth_;
}

int DecoderWorker::height() const
{
    QMutexLocker lock(&mutex_);
    return openHeight_;
}

// ---------------------------------------------------------------------------
// 跨线程投递（线程安全入队 —— 可从任意线程调）
// ---------------------------------------------------------------------------

void DecoderWorker::setOutputSize(const QSize &size)
{
    QMutexLocker lock(&mutex_);
    pendingOutput_    = size;
    hasPendingOutput_ = true;
    wakeup_.wakeOne();
}

void DecoderWorker::decode(const QByteArray &bitstream, bool keyframe)
{
    if (bitstream.isEmpty())
        return;  // 空帧不算错误（与 H264Decoder::decodeFrame 口径一致）

    QMutexLocker lock(&mutex_);
    if (stopping_)
        return;

    // 正处在「丢到下一个关键帧」的丢弃期内：非关键帧直接丢，关键帧才重新开始收。
    if (droppingUntilKeyframe_) {
        if (!keyframe)
            return;
        droppingUntilKeyframe_ = false;
        queue_.clear();   // 关键帧之前的残余（若有）一并作废
    }

    queue_.push_back(PendingFrame{bitstream, keyframe});  // QByteArray 隐式共享
    // 积压超限：砍掉队头，并进入「丢到下一个关键帧」状态。
    // 之所以不是简单丢掉最旧的一帧，是因为随便丢会让后续 P 帧连着解不出来 ——
    // 跳到 IDR 才能让参考链自行恢复。
    // 若刚压进来的这帧本身就是关键帧，则一次到位：清空积压、直接从它重新开始。
    if (queue_.size() > kMaxQueueFrames) {
        if (keyframe) {
            const PendingFrame keep = queue_.back();
            queue_.clear();
            queue_.push_back(keep);
            droppingUntilKeyframe_ = false;
        } else {
            // 队列里通常还压着一个**尚未解码**的关键帧。IDR 不依赖前帧，
            // 从它开始的整段都能正常解 —— 整队清掉会连它一起丢，之后只能干等
            // 下一个 GOP，冻屏白多一整个 GOP。所以只作废它之前的。
            const auto keyIt = std::find_if(queue_.rbegin(), queue_.rend(),
                                            [](const PendingFrame &f) { return f.keyframe; });
            if (keyIt != queue_.rend())
                queue_.erase(queue_.begin(), std::prev(keyIt.base()));

            // 保留之后仍超限 ⇒ 那个关键帧太靠前了（积压整队都是它之后的帧），
            // 留着等于没丢，队列会继续涨。这种只能整队作废、等下一个 IDR。
            if (queue_.size() > kMaxQueueFrames) {
                queue_.clear();
                droppingUntilKeyframe_ = true;
            }
        }
    }
    wakeup_.wakeOne();
}

// ---------------------------------------------------------------------------
// 收尾
// ---------------------------------------------------------------------------

void DecoderWorker::stop()
{
    {
        QMutexLocker lock(&mutex_);
        stopping_ = true;
        // 丢掉未处理的帧：线程都要停了，再解也没人要（也避免 queue_ 里的
        // QByteArray 被拖到下一轮才释放）。
        queue_.clear();
        wakeup_.wakeAll();
    }

    if (!isRunning())
        return;

    // wait() 必须调：QThread 仍活着就被析构 = abort 进程。
    // 解码是同步 MFT，单帧最长几十毫秒，5 s 余量足够。
    if (!wait(5000)) {
        // ⚠️ 不用 terminate()：被强杀的线程不保证清掉 Qt 的 running/finished 记账，
        // 随后 ~QThread 仍可能 qFatal 直接 abort ——「兜底」比多等一会儿糟得多。
        // 退一步：记一笔并继续等（队列已清空且已唤醒，正常会很快退出）。
        qWarning("DecoderWorker: 解码线程 5 s 未退出，继续等待（已放弃 terminate 兜底）");
        wait();
    }
}

} // namespace lanrdp
