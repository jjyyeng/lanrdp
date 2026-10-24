// 收益实测：用**生产路径**（DecoderWorker，一条连接一个解码线程）跑 4 路解码，
// 量化「UI/主线程到底还被占多少」。
//
// 与 bench_decode_multi 的区别：
// bench_decode_multi 用裸 std::thread 验证「多线程方案可行、加速比多少」（可行性压测）；
// 本工具用真实的 DecoderWorker + 真实的「主线程只做入队 + 收帧」路径，量出
// 交付后的主线程占用（验收口径）。
//
// 指标：
// 墙钟 —— 从第一次入队到最后一帧交付的总时间
// 主线程忙时 —— 主线程花在 decode() 入队 + 处理收帧事件上的时间
// UI 占用 —— 主线程忙时 / 墙钟（越低越好；4 路 4K 是 125–155%，物理不可能）
//
// 刻意**不注册 ctest**（要跑几分钟，且是测量而非断言）。
// 用法：bench_decoderworker.exe <输出txt> [宽 高 帧数 重复数 路数]
// 默认 3840x2160 30 帧 × 2 轮 × 4 路

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTextStream>
#include <QThread>
#include <QVector>

#include <cstdio>
#include <cstring>
#include <functional>

#include "host/ColorConvert.h"
#include "host/H264Encoder.h"
#include "viewer/DecoderWorker.h"
#include "viewer/H264Decoder.h"

namespace {

constexpr int kDefaultW       = 3840;
constexpr int kDefaultH       = 2160;
constexpr int kDefaultFrames  = 30;
constexpr int kDefaultRepeats = 2;
constexpr int kDefaultRoads   = 4;

// 有运动的画面 —— 纯静态图会被压成极小帧，解码开销被严重低估
void paintScene(uchar *bgra, int w, int h, int tick)
{
    const int blockW = w / 8;
    const int blockH = h / 8;
    const int bx     = (tick * 37) % (w - blockW);
    const int by     = (tick * 23) % (h - blockH);

    for (int y = 0; y < h; ++y) {
        uchar *row = bgra + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            uchar *p = row + static_cast<size_t>(x) * 4;
            const bool inBlock = (x >= bx && x < bx + blockW && y >= by && y < by + blockH);
            p[0] = static_cast<uchar>(inBlock ? 20 : (x * 255 / w));          // B
            p[1] = static_cast<uchar>(inBlock ? 20 : (y * 255 / h));          // G
            p[2] = static_cast<uchar>(inBlock ? 255 : ((x + y) * 255 / (w + h)));  // R
            p[3] = 0xFF;
        }
    }
}

bool buildStream(int w, int h, int frames, QVector<QByteArray> &out)
{
    lanrdp::H264Encoder enc;
    if (!enc.open(w, h, 30, 20 * 1000 * 1000, 1)) {
        fprintf(stderr, "encoder.open failed: %s\n", qPrintable(enc.lastError()));
        return false;
    }

    QByteArray canvas(static_cast<qsizetype>(w) * h * 4, '\0');
    for (int i = 0; i < frames; ++i) {
        paintScene(reinterpret_cast<uchar *>(canvas.data()), w, h, i);
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(canvas.constData()), w * 4, w, h);
        if (nv12.isEmpty())
            return false;

        QVector<lanrdp::H264EncodedFrame> outs;
        if (!enc.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()), nv12.size(),
                             i == 0, outs))
            return false;
        for (const auto &f : outs)
            out.push_back(f.bitstream);
    }
    QVector<lanrdp::H264EncodedFrame> tail;
    enc.flush(tail);
    for (const auto &f : tail)
        out.push_back(f.bitstream);

    return !out.isEmpty();
}

// 串行基线：同一批包在主线程顺序解码总共产出多少帧（用来决定「跑完」的判据）
int serialFrameCount(const QVector<QByteArray> &packets, int w, int h)
{
    lanrdp::H264Decoder d;
    if (!d.open(w, h))
        return -1;
    int n = 0;
    for (const QByteArray &bs : packets) {
        QVector<QImage> images;
        if (!d.decodeFrame(bs, images))
            return -1;
        n += images.size();
    }
    d.close();
    return n;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString outPath = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QString();
    const int w       = (argc > 2) ? QString::fromLocal8Bit(argv[2]).toInt() : kDefaultW;
    const int h       = (argc > 3) ? QString::fromLocal8Bit(argv[3]).toInt() : kDefaultH;
    const int frames  = (argc > 4) ? QString::fromLocal8Bit(argv[4]).toInt() : kDefaultFrames;
    const int repeats = (argc > 5) ? QString::fromLocal8Bit(argv[5]).toInt() : kDefaultRepeats;
    const int roads   = (argc > 6) ? QString::fromLocal8Bit(argv[6]).toInt() : kDefaultRoads;
    // 第 7 个参数：显示预算的除数（1 = 不缩放，2 = 半尺寸，4 = 四分之一尺寸…）
    // ⚠️ 默认 4 而不是 2：**除数 2 = 面积比 1/4 = 收益曲线上的最差点**（实测 0.53x，
    // 比不缩还慢一倍），函数会刻意回退全尺寸 —— 拿它当默认会量出「预算毫无作用」的假象。
    // 除数 4（面积比 1/16）才落在真省钱的区间（1.62x）。面积比 ≤ 1/9 才值得缩。
    const int div     = (argc > 7) ? QString::fromLocal8Bit(argv[7]).toInt() : 4;

    QTextStream ts(stdout);
    QStringList   log;
    auto          say = [&](const QString &line) {
        ts << line << Qt::endl;
        log << line;
    };

    say(QStringLiteral("=== 生产路径压测：DecoderWorker x %1 (%2x%3) ===")
            .arg(roads).arg(w).arg(h));

    // ---- 造码流 ----
    QElapsedTimer buildTimer;
    buildTimer.start();
    QVector<QByteArray> packets;
    if (!buildStream(w, h, frames, packets)) {
        say(QStringLiteral("造码流失败"));
        return 1;
    }
    say(QStringLiteral("码流: %1 包, 平均 %2 KB/包, 编码耗时 %3 ms")
            .arg(packets.size())
            .arg(int(
                [&] {
                    qint64 t = 0;
                    for (const auto &p : packets) t += p.size();
                    return t / packets.size() / 1024;
                }()))
            .arg(buildTimer.elapsed()));

    const int serial = serialFrameCount(packets, w, h);
    if (serial <= 0) {
        say(QStringLiteral("串行基线解码失败"));
        return 1;
    }
    const int expectedPerRoad = serial * repeats;
    say(QStringLiteral("串行基线: %1 帧/轮 => 每路预期 %2 帧").arg(serial).arg(expectedPerRoad));

    const int totalFramesPlanned = packets.size() * repeats * roads;

    // ---- 4 路 DecoderWorker（生产路径：显示预算 = 格子尺寸口径）----
    // div=1 时不设预算（全尺寸），用来量缩放的收益曲线。
    const QSize budget = (div <= 1) ? QSize()
                                    : QSize(qMax(1, w / div), qMax(1, h / div));
    QVector<lanrdp::DecoderWorker *> workers;
    QVector<int>                     delivered(roads, 0);

    // 主线程为「收帧」付出的成本也要计时。
    //
    // 生产路径每帧不止 decode() 入队：worker 在自己的线程 emit frameDecoded，
    // 排到 UI 线程跑槽里还要 ++rxFrames_、emit frameReady、碰 QImage 引用计数，
    // 外加一次 QMetaCallEvent 分配 —— 这些同样落在主线程上，只是发生在
    // 下面的 processEvents 段。不计这一份，UI 占用会被系统性低估。
    QElapsedTimer slotClock;
    slotClock.start();
    qint64 frameSlotNs = 0;

    for (int i = 0; i < roads; ++i) {
        auto *worker = new lanrdp::DecoderWorker(w, h);
        worker->setOutputSize(budget);
        worker->start();
        if (!worker->waitReady(10000)) {
            say(QStringLiteral("路 %1 解码线程启动失败: %2").arg(i).arg(worker->lastError()));
            return 1;
        }
        QObject::connect(worker, &lanrdp::DecoderWorker::frameDecoded, &app,
                         [&delivered, &frameSlotNs, &slotClock, i](const QImage &image,
                                                                   const QSize &) {
                             const qint64 t0 = slotClock.nsecsElapsed();
                             ++delivered[i];                 // = 生产里的 ++rxFrames_
                             const QSize s = image.size();   // 触碰 QImage（引用计数 + 解引用）
                             (void)s;
                             frameSlotNs += slotClock.nsecsElapsed() - t0;
                         });
        workers.push_back(worker);
    }
    say(QStringLiteral("显示预算: %1（除数 %2）")
            .arg(budget.isValid() ? QStringLiteral("%1x%2").arg(budget.width()).arg(budget.height())
                                  : QStringLiteral("无（全尺寸）"))
            .arg(div));

    // ---- 基准：主线程串行解码（这就是 **之前**的形状：解码全在 UI 线程上）----
    // 与并行组共用同一个墙钟分母，两者可直接比。
    //
    // 量两组：全尺寸（不设预算）与设预算 —— 缩放到底赚不赚，这里直接给出数字。
    auto serialRun = [&](const QSize &outSize) -> double {
        lanrdp::H264Decoder decoder;
        if (!decoder.open(w, h))
            return -1.0;
        if (outSize.isValid())
            decoder.setOutputSize(outSize);
        QElapsedTimer t;
        t.start();
        for (int road = 0; road < roads; ++road) {
            for (int r = 0; r < repeats; ++r) {
                for (const QByteArray &bs : packets) {
                    QVector<QImage> images;  // 每帧新建：decodeFrame 是追加语义
                    decoder.decodeFrame(bs, images);
                }
            }
        }
        const double ms = double(t.nsecsElapsed()) / 1e6;
        decoder.close();
        return ms;
    };

    const double serialFullMs = serialRun(QSize());   // 不缩放
    const double serialMs     = serialRun(budget);    // 缩放路径
    if (serialFullMs < 0 || serialMs < 0) {
        say(QStringLiteral("基准解码器打开失败"));
        return 1;
    }
    say(QStringLiteral("基准(主线程串行 %1 路, 全尺寸)  : %2 ms  (%3 ms/帧)")
            .arg(roads)
            .arg(serialFullMs, 0, 'f', 1)
            .arg(serialFullMs / (totalFramesPlanned * 1.0), 0, 'f', 2));
    say(QStringLiteral("基准(主线程串行 %1 路, %2x%3) : %4 ms  (%5 ms/帧)")
            .arg(roads)
            .arg(budget.width())
            .arg(budget.height())
            .arg(serialMs, 0, 'f', 1)
            .arg(serialMs / (totalFramesPlanned * 1.0), 0, 'f', 2));
    say(QStringLiteral("=> P14 缩放收益  : %1x").arg(serialMs > 0 ? serialFullMs / serialMs : 0.0, 0, 'f', 2));

    // ---- 并行：主线程只做入队 ----
    // 主线程忙时 = decode() 入队耗时（这是主线程为解码付出的**全部**成本）。
    double enqueueMs = 0.0;
    QElapsedTimer clock;
    QElapsedTimer slice;

    clock.start();
    for (int r = 0; r < repeats; ++r) {
        for (const QByteArray &bs : packets) {
            for (auto *worker : workers) {
                slice.start();
                worker->decode(bs);
                enqueueMs += double(slice.nsecsElapsed()) / 1e6;
            }
        }
    }

    // 等所有路收齐。
    // ⚠️ 用 processEvents(WaitForMoreEvents) 阻塞等待，**不要用 msleep** ——
    // Windows 上 msleep(1) 实际会睡 1~15 ms，会把墙钟灌水、帧率被严重低估。
    // 这段是「等」，不计入忙时。
    const int       timeoutMs = 120000;
    QElapsedTimer   wait;
    wait.start();
    for (;;) {
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 5);
        bool done = true;
        for (int i = 0; i < roads; ++i) {
            if (delivered[i] < expectedPerRoad)
                done = false;
        }
        if (done)
            break;
        if (wait.elapsed() > timeoutMs) {
            say(QStringLiteral("等待超时（120 s）"));
            break;
        }
    }
    const double wallMs = double(clock.nsecsElapsed()) / 1e6;

    int totalDelivered = 0;
    for (int i = 0; i < roads; ++i) {
        totalDelivered += delivered[i];
        say(QStringLiteral("  路 %1: 交付 %2 / %3 帧").arg(i).arg(delivered[i]).arg(expectedPerRoad));
    }

    // UI 忙时 = 入队 + 收帧槽两段。只算入队会把占用系统性低估（见上面收帧槽的说明）。
    const double frameSlotMs = double(frameSlotNs) / 1e6;
    const double uiBusyMs    = enqueueMs + frameSlotMs;

    const double uiShare   = wallMs > 0 ? (uiBusyMs / wallMs * 100.0) : 0.0;
    const double baseShare = wallMs > 0 ? (serialMs / wallMs * 100.0) : 0.0;
    const double fps       = wallMs > 0 ? (totalDelivered / wallMs * 1000.0 / roads) : 0.0;
    const double aggFps    = wallMs > 0 ? (totalDelivered / wallMs * 1000.0) : 0.0;
    const double perFrameUs =
        totalDelivered > 0 ? (enqueueMs * 1000.0 / totalDelivered) : 0.0;

    say(QStringLiteral("---------------------------------------------"));
    say(QStringLiteral("墙钟            : %1 ms").arg(wallMs, 0, 'f', 1));
    say(QStringLiteral("主线程忙时(入队): %1 ms  = %2 µs/帧")
            .arg(enqueueMs, 0, 'f', 1)
            .arg(perFrameUs, 0, 'f', 2));
    say(QStringLiteral("主线程忙时(收帧): %1 ms  = %2 µs/帧")
            .arg(frameSlotMs, 0, 'f', 1)
            .arg(totalDelivered > 0 ? frameSlotMs * 1000.0 / totalDelivered : 0.0, 0, 'f', 2));
    say(QStringLiteral("主线程忙时(合计): %1 ms（入队 + 收帧）").arg(uiBusyMs, 0, 'f', 1));
    say(QStringLiteral("=> UI 占用(P15 后): %1 %").arg(uiShare, 0, 'f', 2));
    say(QStringLiteral("=> UI 占用(P15 前): %1 %  （主线程串行解码同等工作量）").arg(baseShare, 0, 'f', 1));
    say(QStringLiteral("=> 每路帧率    : %1 fps（聚合 %2 fps）").arg(fps, 0, 'f', 1).arg(aggFps, 0, 'f', 1));
    say(QStringLiteral("交付总帧数      : %1").arg(totalDelivered));

    for (auto *worker : workers) {
        worker->stop();
        delete worker;
    }

    if (!outPath.isEmpty()) {
        QFile f(outPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(log.join(QLatin1Char('\n')).toUtf8());
    }
    return 0;
}
