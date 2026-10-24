// tests/bench_decode_multi.cpp
//
// 一控多（1 个控制端同时看 N 台被控端）的**解码 + 渲染负载压测**。
//
// 为什么要有这个程序
// ------------------
// 控制端当前是单会话硬编码（MainWindow::startConnect 第一句就是 teardownClient），
// 所以"真的连 4 台"这个场景现在根本跑不起来 —— 端到端压测无从做起。
// 而多格布局一旦落地，N 路 H.264 解码 + N 路缩放绘制会全部挤在 **UI 主线程**上：
// · H264Decoder 是同步 MFT、零线程（代码里搜 QThread 零命中）；
// · ScreenRenderer 走 QPainter raster 软绘，且 smooth_ 默认 true
// （SmoothPixmapTransform = CPU 双线性插值）。
// 在动手重构之前，先用这份压测拿到硬数字，避免"估着做"。
//
// 做法：不重复造轮子 —— 直接用项目自己的编解码器闭环
// --------------------------------------------------
// 合成"桌面感"画面 -> H264Encoder 编成真实码流库 -> N 个 H264Decoder 交错解码计时
// 渲染则是直接量 QPainter 把解码结果画进"格子尺寸"的耗时（smooth 开 / 关两档）
//
// 合成画面为什么不省事用纯色块：
// 双色块编出来的码流极小、解码快得离谱，会**严重低估**负载。真实桌面的空间频率
// 分布是"大面低频 + 大量中频（图标/表格线）+ 高频（文字笔画）"，必须照这个造。
//
// 注意：这是 benchmark 不是单测 —— 结果依赖机器、耗时较长，故在 CMakeLists 里
// **刻意不注册 add_test**（避免污染"27 个测试目标"的计数口径）。
// 跑法：cmake --build build-release --target bench_decode_multi
// build-release/bench_decode_multi.exe <输出txt路径>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringConverter>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h> // CoInitializeEx（WIN32_LEAN_AND_MEAN 下 windows.h 不带它）

#include "host/ColorConvert.h"
#include "host/H264Encoder.h"
#include "viewer/H264Decoder.h"

namespace {

inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// ---------------------------------------------------------------------------
// 合成"桌面感"画面
// ---------------------------------------------------------------------------
// 底图只造一次，之后每帧 memcpy + 盖一个运动块 —— 既有真实纹理复杂度，
// 又有足够运动量，让 P 帧不至于退化成近似空帧（空帧会让解码耗时虚低）。
void paintBase(uchar *p, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        uchar *row = p + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            int b = 24 + (y * 140) / h;                                 // 低频：斜向渐变
            int g = 56 + (x * 96) / w;
            int r = 88 + ((x + y) * 96) / (w + h);
            if (((x >> 4) ^ (y >> 4)) & 1) { b += 18; g += 15; r += 12; }    // 中频：棋盘
            if (((x * 7 + y * 13) % 11) == 0) { b += 30; g += 30; r += 30; } // 高频：噪点
            uchar *q = row + x * 4;
            q[0] = uchar(clamp255(b));
            q[1] = uchar(clamp255(g));
            q[2] = uchar(clamp255(r));
            q[3] = 255;
        }
    }
}

// 盖一个约占 1/9 屏幕面积的斜纹块，位置每帧跳变 —— 制造大面积残差（逼近满码率）
void stampMovingBlock(uchar *p, int w, int h, int fi)
{
    const int bw = w / 3;
    const int bh = h / 3;
    const int x0 = int((static_cast<long long>(fi) * 47) % (w - bw));
    const int y0 = int((static_cast<long long>(fi) * 31) % (h - bh));
    for (int y = y0; y < y0 + bh; ++y) {
        uchar *row = p + static_cast<size_t>(y) * w * 4;
        for (int x = x0; x < x0 + bw; ++x) {
            uchar *q     = row + x * 4;
            const int t  = ((x - x0) + (y - y0)) & 255;
            q[0] = uchar(255 - t);
            q[1] = uchar(t);
            q[2] = uchar(128 + (t >> 1));
        }
    }
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
struct Stats
{
    double avg = 0, p50 = 0, p95 = 0, max = 0;
};

Stats summarize(QVector<double> v)
{
    Stats s;
    if (v.isEmpty())
        return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double d : v)
        sum += d;
    auto pick = [&](double q) {
        size_t i = size_t(q * double(v.size() - 1) + 0.5);
        if (i >= size_t(v.size()))
            i = v.size() - 1;
        return v[i];
    };
    s.avg = sum / double(v.size());
    s.p50 = pick(0.50);
    s.p95 = pick(0.95);
    s.max = v.last();
    return s;
}

// ---------------------------------------------------------------------------
// 码流库：用项目自己的编码器，按指定码率造一段真实 Annex-B 码流
// ---------------------------------------------------------------------------
struct StreamLib
{
    QVector<QByteArray> frames;
    double              avgFrameBytes = 0;
    double              actualMbps    = 0;
};

bool buildStream(int w, int h, int fps, int bitrateBps, int frameCount, StreamLib &lib)
{
    lanrdp::H264Encoder enc;
    if (!enc.open(w, h, fps, bitrateBps, 1)) {
        fprintf(stderr, "encoder.open failed: %s\n", qPrintable(enc.lastError()));
        return false;
    }

    QByteArray base(w * h * 4, '\0');
    paintBase(reinterpret_cast<uchar *>(base.data()), w, h);
    QByteArray canvas(base.size(), '\0');

    qint64 totalBytes = 0;
    auto   collect = [&](QVector<lanrdp::H264EncodedFrame> &outs) {
        for (const auto &f : outs) {
            lib.frames.push_back(f.bitstream);
            totalBytes += f.bitstream.size();
        }
    };

    for (int i = 0; i < frameCount; ++i) {
        memcpy(canvas.data(), base.constData(), size_t(base.size()));
        stampMovingBlock(reinterpret_cast<uchar *>(canvas.data()), w, h, i);

        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(canvas.constData()), w * 4, w, h);
        if (nv12.isEmpty()) {
            fprintf(stderr, "bgraToNv12 failed at %dx%d\n", w, h);
            return false;
        }

        QVector<lanrdp::H264EncodedFrame> outs;
        if (!enc.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()), nv12.size(),
                             /*forceKeyframe=*/i == 0, outs)) {
            fprintf(stderr, "encodeFrame failed: %s\n", qPrintable(enc.lastError()));
            return false;
        }
        collect(outs);
    }

    QVector<lanrdp::H264EncodedFrame> tail;
    enc.flush(tail);
    collect(tail);

    const int n        = lib.frames.size();
    lib.avgFrameBytes  = n ? double(totalBytes) / double(n) : 0;
    lib.actualMbps     = lib.avgFrameBytes * 8.0 * fps / 1e6;
    return n > 0;
}

// 取一帧真实解码结果（供渲染测试当源图）
QImage decodeSampleFrame(const StreamLib &lib, int w, int h)
{
    lanrdp::H264Decoder dec;
    if (!dec.open(w, h))
        return {};
    for (const QByteArray &bs : lib.frames) {
        QVector<QImage> imgs;
        if (!dec.decodeFrame(bs, imgs))
            return {};
        if (!imgs.isEmpty())
            return imgs.last();
    }
    return {};
}

// ---------------------------------------------------------------------------
// A. 解码压测：N 个 decoder 交错解码，模拟单线程处理 N 路 socket
// ---------------------------------------------------------------------------
struct DecodeRun
{
    Stats  frameMs;   // 单帧解码耗时
    Stats  blockMs;   // "一次事件循环迭代"里把 N 路各解一帧的总耗时 = UI 卡顿量度
    double occ10 = 0; // 每路 10fps 时的主线程占用率（%）
    double occ20 = 0; // 每路 20fps 时
};

// budget = 显示预算（解码输出尺寸上限框）；QSize() = 不缩放（= 旧行为）
bool runDecode(const StreamLib &lib, int nWay, int w, int h, int repeats, DecodeRun &out,
               const QSize &budget = QSize())
{
    QVector<lanrdp::H264Decoder *> decs;
    for (int k = 0; k < nWay; ++k) {
        auto *d = new lanrdp::H264Decoder();
        if (!d->open(w, h)) {
            fprintf(stderr, "decoder.open failed: %s\n", qPrintable(d->lastError()));
            for (auto *q : decs)
                delete q;
            return false;
        }
        d->setOutputSize(budget);
        decs.push_back(d);
    }

    auto decodeOne = [&](int k, const QByteArray &bs) -> double {
        QVector<QImage> imgs;
        QElapsedTimer   t;
        t.start();
        decs[k]->decodeFrame(bs, imgs);
        return double(t.nsecsElapsed()) / 1e6;
    };

    // 预热：每个 decoder 各过一整轮，让 MFT 完成缓冲分配 / 参考帧建立。
    // 不计入统计 —— 首个 I 帧的初始化开销不具代表性。
    for (int k = 0; k < nWay; ++k)
        for (const QByteArray &bs : lib.frames)
            decodeOne(k, bs);

    QVector<double> frameMs, blockMs;
    const int       reserve = lib.frames.size() * repeats;
    frameMs.reserve(reserve * nWay);
    blockMs.reserve(reserve);

    for (int r = 0; r < repeats; ++r) {
        for (const QByteArray &bs : lib.frames) {
            QElapsedTimer block;
            block.start();
            for (int k = 0; k < nWay; ++k)
                frameMs.push_back(decodeOne(k, bs));
            blockMs.push_back(double(block.nsecsElapsed()) / 1e6);
        }
    }

    for (auto *d : decs) {
        d->close();
        delete d;
    }

    out.frameMs = summarize(frameMs);
    out.blockMs = summarize(blockMs);
    // 每路 fps f -> 每秒处理 f 个"N 路各一帧"的块
    // 占用率 = f * blockMs.avg / 1000 * 100% == f * blockMs.avg / 10
    out.occ10 = 10.0 * out.blockMs.avg / 10.0;
    out.occ20 = 20.0 * out.blockMs.avg / 10.0;
    return true;
}

// ---------------------------------------------------------------------------
// B. 渲染压测：QPainter 把画面缩放绘进"格子尺寸"
// ---------------------------------------------------------------------------
// 与 ScreenRenderer::paintEvent 同一套调用（drawImage(target, frame_) +
// SmoothPixmapTransform），差别只是画到离屏 QImage 而非控件表面 ——
// 两者都是 raster 引擎，成本同量级。
double measureScalePaint(const QImage &src, const QSize &target, bool smooth, int iters)
{
    if (src.isNull())
        return -1;
    QImage canvas(target, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    const QRect r(QPoint(0, 0), target);

    {
        QPainter p(&canvas);
        p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
        p.drawImage(r, src);
    }
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < iters; ++i) {
        QPainter p(&canvas);
        p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
        p.drawImage(r, src);
    }
    return double(t.nsecsElapsed()) / 1e6 / double(iters);
}

// ---------------------------------------------------------------------------
// A2. 色彩转换单独计时：NV12 -> ARGB32
// ---------------------------------------------------------------------------
// H264Decoder::emitSample 每解出一帧都会调一次 nv12ToArgb32。4K 一帧要写
// 3840*2160*4 = 33 MB —— 纯内存带宽开销。这一项如果占比可观，就说明
// "解码慢"里有一块其实不是 H.264 解码，而是格式转换，那就有优化空间。
double measureNv12ToArgb32(const QByteArray &nv12, int w, int h, int iters)
{
    QElapsedTimer t;
    t.start();
    bool bad = false;
    for (int i = 0; i < iters; ++i) {
        const QImage img = lanrdp::nv12ToArgb32(
            reinterpret_cast<const uchar *>(nv12.constData()), w, w, h);
        if (img.isNull())
            bad = true;
    }
    return bad ? -1 : double(t.nsecsElapsed()) / 1e6 / double(iters);
}

// ---------------------------------------------------------------------------
// A2b. 对照：色彩转换 + 等比缩小（一次遍历里直接输出到目标尺寸）
// ---------------------------------------------------------------------------
// 与 A2 同样的输入，只是走 nv12ToArgb32Scaled —— 少写的是 (1 - dst/src) 那部分内存。
double measureNv12ToArgb32Scaled(const QByteArray &nv12, int w, int h,
                                 const QSize &box, int iters)
{
    QElapsedTimer t;
    t.start();
    bool bad = false;
    for (int i = 0; i < iters; ++i) {
        const QImage img = lanrdp::nv12ToArgb32Scaled(
            reinterpret_cast<const uchar *>(nv12.constData()), w, w, h,
            box.width(), box.height());
        if (img.isNull())
            bad = true;
    }
    return bad ? -1 : double(t.nsecsElapsed()) / 1e6 / double(iters);
}

// ---------------------------------------------------------------------------
// A3. 多线程解码对照
// ---------------------------------------------------------------------------
// 关键约束（读代码得到，不是推测）：
// · MFTEnumEx 只要 MFT_ENUM_FLAG_SYNCMFT —— 是同步软解 MFT，不用 GPU；
// · MfRuntime::ensureMfRuntime 用 CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)
// = 主线程 STA，且 std::call_once 只初始化一次；
// · 同步 MFT 有线程亲和性。
// 所以多线程方案必须**在工作线程内** CoInitializeEx + open()，不能跨线程搬运。
// 本实验就是验证这条路走不走得通、能拿到多少加速比。
double runDecodeSerialN(const StreamLib &lib, int nWay, int w, int h, int repeats)
{
    QVector<lanrdp::H264Decoder *> decs;
    for (int k = 0; k < nWay; ++k) {
        auto *d = new lanrdp::H264Decoder();
        if (!d->open(w, h)) {
            delete d;
            for (auto *q : decs) {
                q->close();
                delete q;
            }
            return -1;
        }
        decs.push_back(d);
    }

    // 每帧都新开一个 QVector<QImage> —— 与真实路径一致（H264Decoder 每帧返回新
    // QImage）。绝不能把 imgs 放到循环外累积：4K 单帧 33 MB，几百帧就是几十 GB。
    for (int k = 0; k < nWay; ++k) // 预热
        for (const QByteArray &bs : lib.frames) {
            QVector<QImage> imgs;
            decs[k]->decodeFrame(bs, imgs);
        }

    QElapsedTimer t;
    t.start();
    for (int r = 0; r < repeats; ++r)
        for (const QByteArray &bs : lib.frames)
            for (int k = 0; k < nWay; ++k) {
                QVector<QImage> imgs;
                decs[k]->decodeFrame(bs, imgs);
            }
    const double ms = double(t.nsecsElapsed()) / 1e6;

    for (auto *d : decs) {
        d->close();
        delete d;
    }
    return ms;
}

struct MtResult
{
    double  wallMs     = 0;
    double  perRoadFps = 0; // 折算：每路每秒能解多少帧
    QString err;
};

bool runDecodeMultiThreaded(const StreamLib &lib, int nWay, int w, int h, int repeats,
                            MtResult &out)
{
    QVector<lanrdp::H264Decoder *> decs(nWay, nullptr);
    QVector<QString>               errs(nWay);

    std::atomic<int>         warmed{0};
    std::atomic<bool>        go{false};
    std::vector<std::thread> ths;
    ths.reserve(size_t(nWay));

    const int framesPerThread = lib.frames.size() * repeats;

    for (int k = 0; k < nWay; ++k) {
        ths.emplace_back([&, k]() {
            // 工作线程必须自己初始化 COM（主线程那次 call_once 不覆盖本线程）
            ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            auto *d = new lanrdp::H264Decoder();
            if (!d->open(w, h)) {
                errs[k] = d->lastError();
                delete d;
                warmed.fetch_add(1);
                return;
            }
            decs[k] = d;

            for (const QByteArray &bs : lib.frames) { // 预热
                QVector<QImage> imgs;
                d->decodeFrame(bs, imgs);
            }

            warmed.fetch_add(1);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (int r = 0; r < repeats; ++r)
                for (const QByteArray &bs : lib.frames) {
                    QVector<QImage> imgs;
                    d->decodeFrame(bs, imgs);
                }
        });
    }

    while (warmed.load() < nWay)
        std::this_thread::yield();

    QElapsedTimer wall;
    wall.start();
    go.store(true, std::memory_order_release);

    for (auto &t : ths)
        t.join();

    out.wallMs = double(wall.nsecsElapsed()) / 1e6;

    for (int k = 0; k < nWay; ++k) {
        if (!errs[k].isEmpty())
            out.err = errs[k];
        if (decs[k]) {
            decs[k]->close();
            delete decs[k];
            decs[k] = nullptr;
        }
    }

    if (out.wallMs > 0 && framesPerThread > 0)
        out.perRoadFps = double(framesPerThread) / out.wallMs * 1000.0;
    return out.err.isEmpty();
}

QString num(double v, int prec = 2)
{
    return QString::number(v, 'f', prec);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString outPath = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral("bench_decode_multi.txt");

    // 可选：第 2 个参数只跑指定小节（逗号分隔，如 "a2,a4"），便于迭代时避开
    // A / A3 两节（各要几分钟）。不给 = 全跑。
    const QString only = (argc > 2) ? QString::fromLocal8Bit(argv[2]).toLower().trimmed()
                                    : QString();
    auto want = [&only](const QString &sec) {
        return only.isEmpty() || only == QStringLiteral("all")
               || only.split(QLatin1Char(',')).contains(sec);
    };

    QFile outFile(outPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        fprintf(stderr, "cannot open output: %s\n", qPrintable(outPath));
        return 2;
    }
    QTextStream os(&outFile);
    os.setEncoding(QStringConverter::Utf8);
    auto say = [&](const QString &s) {
        os << s << '\n';
        os.flush();
        fprintf(stdout, "%s\n", s.toLocal8Bit().constData());
    };

    const int    kFrames  = 30;
    const int    kRepeats = 3;
    const int    kFps     = 10;
    const int    kNWays[] = {1, 2, 3, 4};
    const int    kRates[] = {4 * 1000 * 1000, 8 * 1000 * 1000, 16 * 1000 * 1000};

    struct Res
    {
        int         w, h;
        const char *name;
    };
    const Res kResList[] = {{1920, 1080, "1920x1080"}, {3840, 2160, "3840x2160"}};

    // 渲染目标尺寸：按"控制端中央区约 900x600 逻辑 px"折算
    // 1x1 -> 整块 900x600
    // 1x2 -> 每格 450x600
    // 2x2 / 3台 -> 每格 450x300（最小格子 = 最坏情况）
    const int kPaintIter = 20;

    say(QStringLiteral("================ 一控多 · 解码 + 渲染负载压测 ================"));
    say(QStringLiteral("机器  : 本机（3840x2160 @150%，Windows）"));
    say(QStringLiteral("帧数  : 每档码流 %1 帧；解码统计重复 %2 轮（另跑 1 轮预热不计）")
            .arg(kFrames).arg(kRepeats));
    say(QStringLiteral("说明  : 合成画面 = 低频渐变 + 中频棋盘 + 高频噪点 + 1/9 面积运动块"));
    if (!only.isEmpty())
        say(QStringLiteral("小节  : 只跑 [%1]（第 2 个参数；可选 a / a2 / a3 / a4 / b）").arg(only));
    say(QStringLiteral(""));

    // ---------- A. 解码 ----------
    say(QStringLiteral("---------------- A. 解码耗时（UI 主线程，同步 MFT 软解） ----------------"));
    say(QString());
    say(QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8")
            .arg(QStringLiteral("分辨率"), -10)
            .arg(QStringLiteral("目标Mbps"), 9)
            .arg(QStringLiteral("实测Mbps"), 9)
            .arg(QStringLiteral("N"), 3)
            .arg(QStringLiteral("单帧ms avg/p50/p95/max"), -25)
            .arg(QStringLiteral("块ms avg/max"), -16)
            .arg(QStringLiteral("占用@10fps"), 11)
            .arg(QStringLiteral("占用@20fps"), 11));

    QVector<QString> verdicts;

    for (const Res &res : kResList) {
        if (!want(QStringLiteral("a")))
            continue;
        for (int rate : kRates) {
            StreamLib lib;
            if (!buildStream(res.w, res.h, kFps, rate, kFrames, lib)) {
                say(QStringLiteral("!! %1 @%2Mbps 码流生成失败").arg(res.name).arg(rate / 1e6));
                continue;
            }

            for (int n : kNWays) {
                DecodeRun run;
                if (!runDecode(lib, n, res.w, res.h, kRepeats, run)) {
                    say(QStringLiteral("!! %1 N=%2 解码失败").arg(res.name).arg(n));
                    continue;
                }

                const QString frameCol
                    = QStringLiteral("%1 / %2 / %3 / %4")
                          .arg(num(run.frameMs.avg), num(run.frameMs.p50),
                               num(run.frameMs.p95), num(run.frameMs.max));
                const QString blockCol = QStringLiteral("%1 / %2")
                                             .arg(num(run.blockMs.avg), num(run.blockMs.max));

                say(QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8")
                        .arg(QString::fromLatin1(res.name), -10)
                        .arg(num(rate / 1e6, 0), 9)
                        .arg(num(lib.actualMbps, 2), 9)
                        .arg(n, 3)
                        .arg(frameCol, -25)
                        .arg(blockCol, -16)
                        .arg(num(run.occ10, 1) + QStringLiteral("%"), 11)
                        .arg(num(run.occ20, 1) + QStringLiteral("%"), 11));

                if (n == 4) {
                    verdicts << QStringLiteral("%1 @实测%2Mbps  4路@10fps 占用 %3%% | 4路@20fps 占用 %4%% | 最坏单次阻塞 %5ms")
                                    .arg(QString::fromLatin1(res.name), num(lib.actualMbps, 1),
                                         num(run.occ10, 1), num(run.occ20, 1),
                                         num(run.blockMs.max));
                }
            }
            say(QString());
        }
    }

    // ---------- A2. 色彩转换（全尺寸 vs 「转换 + 等比缩小」） ----------
    //
    // 目标框按本机实际窗口算：屏幕 3840x2160 @150% ⇒ 逻辑 2560x1440；
    // 左边操作面板落定 268 逻辑 px，再去掉菜单/状态栏 ⇒ 中央区约 2280x1380 逻辑，
    // 换算成**设备像素**是 3420x2070（预算用物理像素，见
    // MainWindow::pushDisplayBudget —— 给逻辑像素会让画面被放大 1.5 倍变糊）。
    // 另外列出 2280x1380（若改用逻辑像素）与 1710x1035（2x2 布局的单格物理尺寸）
    // 作参照，方便判断「预算取中央区还是取格子」的取舍。
    const QSize kBoxes[] = {{0, 0}, {3420, 2070}, {2280, 1380}, {1710, 1035}};

    // 迭代次数按像素数定，保证每次测量至少跑 ~1 秒。**别图省事写死 20 次**：
    // 1080p 一次转换才 6 ms，20 次一共 120 ms，测出来在同一份代码路径上能漂 ±40%
    // （实测「不缩放的同一路径」给出 5.71 / 8.24 / 6.06 / 6.78 ms 四个不同值）。
    const auto itersFor = [](int w, int h) {
        return qMax(20, static_cast<int>(4.0e8 / double(static_cast<qint64>(w) * h)));
    };

    say(QStringLiteral("---------------- A2. 色彩转换单独计时（H264Decoder 每解出一帧就做一次） ----------------"));
    say(QStringLiteral("目标框：全尺寸 = 旧行为 ｜ 3420x2070 = 本机中央区(物理) ｜ 2280x1380 = 本机中央区(逻辑) ｜ 1710x1035 = 2x2 单格(物理)"));
    say(QString());
    say(QStringLiteral("%1 %2 %3 %4 %5 %6")
            .arg(QStringLiteral("分辨率"), -10)
            .arg(QStringLiteral("目标框"), -12)
            .arg(QStringLiteral("输出尺寸"), -12)
            .arg(QStringLiteral("ms/帧"), 9)
            .arg(QStringLiteral("4路@10fps占用"), 15)
            .arg(QStringLiteral("相对全尺寸"), 12));

    QVector<QString> p14Verdicts;
    for (const Res &res : kResList) {
        if (!want(QStringLiteral("a2")))
            continue;
        QByteArray base(res.w * res.h * 4, '\0');
        paintBase(reinterpret_cast<uchar *>(base.data()), res.w, res.h);
        stampMovingBlock(reinterpret_cast<uchar *>(base.data()), res.w, res.h, 7);
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(base.constData()), res.w * 4, res.w, res.h);

        const int    iters  = itersFor(res.w, res.h);
        const double baseMs = measureNv12ToArgb32(nv12, res.w, res.h, iters);

        for (const QSize &box : kBoxes) {
            const bool     full = box.isEmpty();
            const double   ms   = full ? baseMs
                                       : measureNv12ToArgb32Scaled(nv12, res.w, res.h, box, iters);
            const QImage   probe = full ? QImage()
                                        : lanrdp::nv12ToArgb32Scaled(
                                              reinterpret_cast<const uchar *>(nv12.constData()),
                                              res.w, res.w, res.h, box.width(), box.height());
            const QString outSize = full ? QStringLiteral("%1x%2").arg(res.w).arg(res.h)
                                         : QStringLiteral("%1x%2").arg(probe.width()).arg(probe.height());
            const double  occ  = 4.0 * ms;  // 4 路 * 10fps
            const double  gain = (ms > 0) ? baseMs / ms : 0;

            say(QStringLiteral("%1 %2 %3 %4 %5 %6")
                    .arg(QString::fromLatin1(res.name), -10)
                    .arg(full ? QStringLiteral("全尺寸") : QStringLiteral("%1x%2").arg(box.width()).arg(box.height()), -12)
                    .arg(outSize, -12)
                    .arg(num(ms), 9)
                    .arg(num(occ, 1) + QStringLiteral("%"), 15)
                    .arg(num(gain, 2) + QStringLiteral("x"), 12));

            if (box == QSize(3420, 2070)) {
                p14Verdicts << QStringLiteral("%1 色彩转换：全尺寸 %2ms -> 中央区(物理) %3ms（%4x），4路@10fps 占用 %5% -> %6%")
                                   .arg(QString::fromLatin1(res.name), num(baseMs), num(ms),
                                        num(gain, 2), num(4.0 * baseMs, 1), num(occ, 1));
            }
        }
        say(QString());
    }

    // ---------- A2b. 缩放收益的拐点扫描（**定阈值用**，与 A2 分开跑） ----------
    //
    // 背景：守卫写的是「输出面积 > 源 1/4 就回退全尺寸」，而 `>` 让**恰好 1/4**
    // 落进缩放路径 —— 实测那一点比不缩还慢一倍（压测复现），是唯一的反向异常点。
    // 拐点到底在哪，靠扫描定：这里按**面积比**反推目标框，只扫 ≤1/4 的区间
    // （再大的比例连旧守卫都不放行，已测 0.79 → 0.68x、0.35 → 0.94x，都亏）。
    //
    // 只跑本节：`bench_decode_multi.exe <out> a2b`（约 2 分钟）。
    if (want(QStringLiteral("a2b"))) {
        struct Ratio {
            const char *label;
            int         num;
            int         den;
        };
        const Ratio kRatios[] = {{"1/4", 1, 4},   {"1/5", 1, 5},   {"1/6", 1, 6},
                                 {"1/8", 1, 8},   {"1/9", 1, 9},   {"1/12", 1, 12},
                                 {"1/16", 1, 16}, {"1/64", 1, 64}};

        say(QStringLiteral("---------------- A2b. 缩放收益拐点扫描（纯色彩转换口径） ----------------"));
        say(QStringLiteral("目标框按面积比反推（等比缩放 s = sqrt(比)），基准 = 同分辨率全尺寸转换"));
        say(QStringLiteral("输出尺寸 = 全尺寸 ⇒ 那一档被收益守卫回退了（阈值 1/9，是**有意**的不缩，不是失败）"));
        say(QString());
        say(QStringLiteral("%1 %2 %3 %4 %5 %6")
                .arg(QStringLiteral("分辨率"), -10)
                .arg(QStringLiteral("目标面积比"), -12)
                .arg(QStringLiteral("输出尺寸"), -12)
                .arg(QStringLiteral("实际比"), 10)
                .arg(QStringLiteral("ms/帧"), 9)
                .arg(QStringLiteral("相对全尺寸"), 12));

        for (const Res &res : kResList) {
            QByteArray base(res.w * res.h * 4, '\0');
            paintBase(reinterpret_cast<uchar *>(base.data()), res.w, res.h);
            stampMovingBlock(reinterpret_cast<uchar *>(base.data()), res.w, res.h, 7);
            const QByteArray nv12 = lanrdp::bgraToNv12(
                reinterpret_cast<const uchar *>(base.constData()), res.w * 4, res.w, res.h);

            const int    iters  = itersFor(res.w, res.h);
            const double baseMs = measureNv12ToArgb32(nv12, res.w, res.h, iters);

            for (const Ratio &r : kRatios) {
                const double s  = std::sqrt(double(r.num) / double(r.den));
                const int    bw = qMax(2, static_cast<int>(std::lround(res.w * s)));
                const int    bh = qMax(2, static_cast<int>(std::lround(res.h * s)));

                const QImage probe = lanrdp::nv12ToArgb32Scaled(
                    reinterpret_cast<const uchar *>(nv12.constData()), res.w, res.w, res.h, bw, bh);

                // ⚠️ 整数取整 + 「输出面积超阈值就回退全尺寸」两条都会让实际输出 ≠ 目标框，
                // 所以必须把**真实输出尺寸**和**真实面积比**一起打出来，否则读表会读错。
                const double actual = (double(probe.width()) * probe.height())
                                      / (double(res.w) * res.h);
                const double ms   = measureNv12ToArgb32Scaled(nv12, res.w, res.h, QSize(bw, bh), iters);
                const double gain = (ms > 0) ? baseMs / ms : 0;

                say(QStringLiteral("%1 %2 %3 %4 %5 %6")
                        .arg(QString::fromLatin1(res.name), -10)
                        .arg(QString::fromLatin1(r.label), -12)
                        .arg(QStringLiteral("%1x%2").arg(probe.width()).arg(probe.height()), -12)
                        .arg(num(actual, 3), 10)
                        .arg(num(ms), 9)
                        .arg(num(gain, 2) + QStringLiteral("x"), 12));
            }
            say(QString());
        }
    }

    // ---------- A4. 端到端对照：解码整链（含转换）有/无显示预算 ----------
    say(QStringLiteral("---------------- A4. 解码整链对照（decodeFrame，含色彩转换；N=4） ----------------"));
    say(QStringLiteral("预算按**面积比**给（不是窗口几何）：1/4 = 收益曲线最差点（守卫会回退成全尺寸），1/16 = 真省钱的档"));
    say(QString());
    say(QStringLiteral("%1 %2 %3 %4 %5 %6")
            .arg(QStringLiteral("分辨率"), -10)
            .arg(QStringLiteral("显示预算"), -12)
            .arg(QStringLiteral("单帧ms avg/p95/max"), -22)
            .arg(QStringLiteral("4路块ms avg"), 12)
            .arg(QStringLiteral("4路@10fps"), 11)
            .arg(QStringLiteral("提速"), 8));

    // 面积比 -> 上限框：等比缩放因子 s = sqrt(面积比)
    const struct
    {
        const char *label;
        double      area;
    } kBudgets[] = {{"无(全尺寸)", 0.0}, {"1/4 面积", 0.25}, {"1/16 面积", 0.0625}};

    for (const Res &res : kResList) {
        if (!want(QStringLiteral("a4")))
            continue;
        StreamLib lib;
        if (!buildStream(res.w, res.h, kFps, 8 * 1000 * 1000, kFrames, lib))
            continue;

        double baseSingle = 0;  // 该分辨率下「无预算」的单帧耗时，做提速比的基准
        for (const auto &b : kBudgets) {
            const QSize box = (b.area <= 0.0)
                                  ? QSize()
                                  : QSize(qMax(2, static_cast<int>(
                                                   std::lround(res.w * std::sqrt(b.area)))),
                                          qMax(2, static_cast<int>(
                                                   std::lround(res.h * std::sqrt(b.area)))));
            DecodeRun run;
            if (!runDecode(lib, 4, res.w, res.h, kRepeats, run, box)) {
                say(QStringLiteral("!! %1 预算%2 解码失败")
                        .arg(QString::fromLatin1(res.name)).arg(box.width()));
                continue;
            }
            if (box.isEmpty())   // kBoxes 首项就是基线
                baseSingle = run.frameMs.avg;

            const double  gain = (baseSingle > 0 && run.frameMs.avg > 0)
                                     ? baseSingle / run.frameMs.avg : 0;
            const QString frameCol = QStringLiteral("%1 / %2 / %3")
                                         .arg(num(run.frameMs.avg), num(run.frameMs.p95),
                                              num(run.frameMs.max));
            say(QStringLiteral("%1 %2 %3 %4 %5 %6")
                    .arg(QString::fromLatin1(res.name), -10)
                    .arg(box.isEmpty() ? QStringLiteral("无(全尺寸)")
                                       : QStringLiteral("%1(%2x%3)").arg(QString::fromUtf8(b.label))
                                             .arg(box.width()).arg(box.height()), -12)
                    .arg(frameCol, -22)
                    .arg(num(run.blockMs.avg), 12)
                    .arg(num(run.occ10, 1) + QStringLiteral("%"), 11)
                    .arg(baseSingle > 0 ? num(gain, 2) + QStringLiteral("x") : QStringLiteral("-"), 8));
        }
        say(QString());
    }

    // ---------- A3. 多线程 ----------
    say(QStringLiteral("---------------- A3. 多线程解码对照（每路一个工作线程，各自线程内 open） ----------------"));
    say(QStringLiteral("串行 = 同一份工作量在单线程顺序解；并行 = 每路一个工作线程同时解。"));
    say(QString());
    say(QStringLiteral("%1 %2 %3 %4 %5 %6")
            .arg(QStringLiteral("分辨率"), -10)
            .arg(QStringLiteral("N"), 3)
            .arg(QStringLiteral("串行ms"), 9)
            .arg(QStringLiteral("并行ms"), 9)
            .arg(QStringLiteral("加速比"), 8)
            .arg(QStringLiteral("每路fps能力"), 12));

    for (const Res &res : kResList) {
        if (!want(QStringLiteral("a3")))
            continue;
        StreamLib lib;
        if (!buildStream(res.w, res.h, kFps, 8 * 1000 * 1000, kFrames, lib))
            continue;
        for (int n : kNWays) {
            const double serialMs = runDecodeSerialN(lib, n, res.w, res.h, kRepeats);
            MtResult     mt;
            if (!runDecodeMultiThreaded(lib, n, res.w, res.h, kRepeats, mt)) {
                say(QStringLiteral("!! %1 N=%2 多线程失败：%3")
                        .arg(QString::fromLatin1(res.name))
                        .arg(n)
                        .arg(mt.err));
                continue;
            }
            const double speedup = (serialMs > 0) ? serialMs / mt.wallMs : 0;
            say(QStringLiteral("%1 %2 %3 %4 %5 %6")
                    .arg(QString::fromLatin1(res.name), -10)
                    .arg(n, 3)
                    .arg(num(serialMs, 0), 9)
                    .arg(num(mt.wallMs, 0), 9)
                    .arg(num(speedup, 2) + QStringLiteral("x"), 8)
                    .arg(num(mt.perRoadFps, 1) + QStringLiteral(" fps"), 12));
        }
        say(QString());
    }

    // ---------- B. 渲染 ----------
    say(QStringLiteral("---------------- B. 缩放绘制耗时（QPainter raster，模拟 ScreenRenderer） ----------------"));
    say(QString());
    say(QStringLiteral("目标尺寸 = P18 真实格子（设备像素；本机中央区 3420x2070 按 layoutForCount 均分）"));
    say(QStringLiteral("* 4K 源 + 1710x1035 单格（2x2）= 面积比 0.2134，落 (1/9,1/4] 死区："));
    say(QStringLiteral("  解码侧守卫回退 => 这一跳由 QPainter 承担。量它就是为了看死区到底多贵。"));
    say(QString());
    say(QStringLiteral("%1 %2 %3 %4 %5 %6")
            .arg(QStringLiteral("源分辨率"), -10)
            .arg(QStringLiteral("目标格子"), -16)
            .arg(QStringLiteral("平滑"), -6)
            .arg(QStringLiteral("ms/帧"), 8)
            .arg(QStringLiteral("4格@10fps占用"), 14)
            .arg(QStringLiteral("4格@20fps占用"), 14));

    struct Target
    {
        QSize       size;
        const char *name;
    };
    // ⚠️ 2026-09-25 改口径：**改为真实格子尺寸（设备像素）**。
    // 旧的 900x600 / 450x600 / 450x300 是「逻辑像素年代」的估计值，与本机
    // 4K@150% 的实际格子（3420x2070 中央区均分）差了一倍多，读表会读错。
    // 现在的对照就是 `layoutForCount` 的四个档位在本机的真实落位。
    // （单一来源：ViewGrid::layoutForCount + 中央区物理尺寸）
    // ⚠️ 宽度给 -10 会把「2x2/3台」这样的多字节标签截成半个 UTF-8 字符（实测出现
    // `3å°`）。中文表头的宽度必须按**字节数**留够 —— 这里统一放到 -16。
    const Target kTargets[] = {
        {{3420, 2070}, "1x1   3420x2070"},
        {{1710, 2070}, "1x2   1710x2070"},
        {{1710, 1035}, "2x2   1710x1035"},
    };

    for (const Res &res : kResList) {
        if (!want(QStringLiteral("b")))
            continue;
        StreamLib lib;
        if (!buildStream(res.w, res.h, kFps, 8 * 1000 * 1000, kFrames, lib))
            continue;
        const QImage sample = decodeSampleFrame(lib, res.w, res.h);
        if (sample.isNull()) {
            say(QStringLiteral("!! %1 取样本帧失败").arg(res.name));
            continue;
        }

        QVector<Target> targets;
        for (const Target &t : kTargets)
            targets.append(t);
        // （原有的 1920x1080* 特例行已删除：它量的是「恰好 1/4 面积」，
        // 而本机 2x2 真实单格是 1710x1035 / 面积比 0.2134 —— 直接落在
        // kTargets 的第三档里，不必再单列一行混淆读表。）

        for (const Target &tg : targets) {
            for (int smooth = 0; smooth < 2; ++smooth) {
                const double ms = measureScalePaint(sample, tg.size, smooth != 0, kPaintIter);
                const double o10 = 4 * 10 * ms / 10.0;
                const double o20 = 4 * 20 * ms / 10.0;
                say(QStringLiteral("%1 %2 %3 %4 %5 %6")
                        .arg(QString::fromLatin1(res.name), -10)
                        .arg(QString::fromLatin1(tg.name), -10)
                        .arg(smooth ? QStringLiteral("开") : QStringLiteral("关"), -6)
                        .arg(num(ms), 8)
                        .arg(num(o10, 1) + QStringLiteral("%"), 14)
                        .arg(num(o20, 1) + QStringLiteral("%"), 14));
            }
        }
        say(QString());
    }

    // ---------- C. 摘要 ----------
    say(QStringLiteral("---------------- C. 摘要（4 路时） ----------------"));
    say(QString());
    for (const QString &v : verdicts)
        say(QStringLiteral("  · ") + v);
    say(QString());
    for (const QString &v : p14Verdicts)
        say(QStringLiteral("  · [色彩转换带缩放] ") + v);
    say(QString());
    say(QStringLiteral("※ 判读（色彩转换带缩放）：省掉的是「写全尺寸 ARGB32」这一步（4K 一帧 33 MB），"));
    say(QStringLiteral("   不是 H.264 解码本体。收益 = 1 - 目标面积/源码面积，所以"));
    say(QStringLiteral("   本地屏比远端屏小时收益最大；两者同为 4K 时 1x1 几乎不缩（不该缩）。"));
    say(QStringLiteral("   收益判定阈值 = 面积比 1/9 —— 定档依据是**关掉守卫**时单测缩放路径本身的"));
    say(QStringLiteral("   收益（1080p / 4K 两条曲线几乎重合）：1/8 -> 0.94x（亏）、1/9 -> 1.06x（微赚）。"));
    say(QStringLiteral("   开启守卫后 >1/9 一律回退全尺寸，把那点缩放交给渲染（见 B 节 1920x1080* 行）。"));
    say(QString());
    say(QStringLiteral("※ 判读提示：占用率是「主线程被解码占掉的比例」。"));
    say(QStringLiteral("   >25% 时界面开始可见迟滞；>50% 时拖窗口/点按钮明显卡；"));
    say(QStringLiteral("   接近 100% 意味着画面会持续落后、事件积压。"));
    say(QStringLiteral("   注意「最坏单次阻塞」才是用户直接感知的卡顿源 —— 它是一次事件循环里"));
    say(QStringLiteral("   N 路帧同时到达时的串行解码总时长。"));

    os.flush();
    outFile.close();
    fprintf(stdout, "\n[done] -> %s\n", qPrintable(outPath));
    return 0;
}
