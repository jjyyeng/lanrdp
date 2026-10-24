// tests/bench_adaptive_bitrate.cpp
//
// 可行性实验：Media Foundation 的**同步** H.264 编码器能否在**运行中**即时改码率
// --------------------------------------------------------------------------------
// 「自适应码率」的立项前置实验。要回答的唯一技术未知项是：
// 对已打开、正在编码的 H264Encoder 再设一次 CODECAPI_AVEncCommonMeanBitRate，
// 输出码率是**立刻**下降，还是要等下个 GOP 边界 / FLUSH，甚至根本不生效。
//
// 结论直接决定做法：
// · 即时生效 → 立项（运行中调码率 + 反馈帧）
// · 延迟/GOP → 也立项，但要接受「秒级生效」的语义
// · 不生效 → 退化为「只调 fps + GOP」，或「重建编码器换码率」
//
// 做法（与 bench_decode_multi 同源）：
// 合成"桌面感"画面 -> 同一个 encoder 实例：
// A. open(8Mbps) 送 30 帧 —— 高码率基线
// B. setBitrate(1Mbps) 送 90 帧 —— 运行中降码率，按产出顺序分桶看收敛
// C. 另起一个 encoder open(1Mbps) 送 30 帧 —— 干净低码率对照
// 对比 B 的稳态桶 vs C，判断「运行中改」是否真的到达目标码率。
//
// 注意：这是 benchmark 不是单测 —— 结果依赖机器，故在 CMakeLists 里**不注册
// add_test**（不污染测试目标计数）。跑法见 CMakeLists 末尾注释。

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QString>
#include <QStringConverter>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "host/ColorConvert.h"
#include "host/H264Encoder.h"

namespace {

inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// 合成"桌面感"底图：低频渐变 + 中频棋盘 + 高频噪点（与 bench_decode_multi 同款）
void paintBase(uchar *p, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        uchar *row = p + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            int b = 24 + (y * 140) / h;
            int g = 56 + (x * 96) / w;
            int r = 88 + ((x + y) * 96) / (w + h);
            if (((x >> 4) ^ (y >> 4)) & 1) { b += 18; g += 15; r += 12; }
            if (((x * 7 + y * 13) % 11) == 0) { b += 30; g += 30; r += 30; }
            uchar *q = row + x * 4;
            q[0] = uchar(clamp255(b));
            q[1] = uchar(clamp255(g));
            q[2] = uchar(clamp255(r));
            q[3] = 255;
        }
    }
}

// 盖一个约 1/9 屏幕面积的斜纹运动块，制造大面积残差（逼近满码率）
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

struct Segment
{
    QVector<qint64> bytes;      // 每帧码流字节数（按产出顺序）
    QVector<bool>   keyframes;  // 对应帧是否关键帧
    double avgBytes  = 0;
    double avgMbps   = 0;
    qint64 keyBytes  = 0;       // 关键帧平均字节数
    int    keyCount  = 0;
};

// 送 frameCount 帧到 encoder，收集产出帧的字节数与关键帧标记。
// 不 flush —— 保持编码器打开，供"运行中改码率"后继续用同一实例。
// fps 不在形参里：它已在 enc.open() 时定下，送帧这步用不到。
bool runSegment(lanrdp::H264Encoder &enc, int w, int h, int frameCount,
                int startIndex, bool forceFirstKeyframe, Segment &out, QString &err)
{
    QByteArray base(w * h * 4, '\0');
    paintBase(reinterpret_cast<uchar *>(base.data()), w, h);
    QByteArray canvas(base.size(), '\0');

    for (int i = 0; i < frameCount; ++i) {
        memcpy(canvas.data(), base.constData(), size_t(base.size()));
        stampMovingBlock(reinterpret_cast<uchar *>(canvas.data()), w, h, startIndex + i);

        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(canvas.constData()), w * 4, w, h);
        if (nv12.isEmpty()) {
            err = QStringLiteral("bgraToNv12 失败 @%1x%2").arg(w).arg(h);
            return false;
        }

        QVector<lanrdp::H264EncodedFrame> outs;
        if (!enc.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()), nv12.size(),
                             forceFirstKeyframe && i == 0, outs)) {
            err = enc.lastError();
            return false;
        }
        for (const auto &f : outs) {
            out.bytes.append(f.bitstream.size());
            out.keyframes.append(f.keyframe);
        }
    }
    return true;
}

void summarize(Segment &s, int fps)
{
    if (s.bytes.isEmpty())
        return;
    double sum = 0;
    qint64 ksum = 0;
    int    kcnt = 0;
    for (int i = 0; i < s.bytes.size(); ++i) {
        sum += double(s.bytes[i]);
        if (s.keyframes[i]) { ksum += s.bytes[i]; ++kcnt; }
    }
    s.avgBytes = sum / double(s.bytes.size());
    s.avgMbps  = s.avgBytes * 8.0 * double(fps) / 1e6;
    s.keyBytes = kcnt ? ksum / kcnt : 0;
    s.keyCount = kcnt;
}

QString num(double v, int prec = 1)
{
    return QString::number(v, 'f', prec);
}

// 对 out.bytes 的一段（按产出顺序）算平均字节数与折算码率
void bucketAvg(const Segment &s, int from, int to, int fps, double &avgBytes, double &mbps)
{
    avgBytes = 0;
    mbps     = 0;
    if (from < 0)
        from = 0;
    if (to > s.bytes.size())
        to = s.bytes.size();
    if (to <= from)
        return;
    double sum = 0;
    for (int i = from; i < to; ++i)
        sum += double(s.bytes[i]);
    avgBytes = sum / double(to - from);
    mbps     = avgBytes * 8.0 * double(fps) / 1e6;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString outPath = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral("bench_adaptive_bitrate.txt");

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

    const int w   = 1920;
    const int h   = 1080;
    const int fps = 30;
    const int highBps = 8 * 1000 * 1000;
    const int lowBps  = 1 * 1000 * 1000;

    say(QStringLiteral("================ ⑨-0 运行中改码率可行性实验 ================"));
    say(QStringLiteral("分辨率 %1x%2 @%3fps ｜ 高码率 %4Mbps → 运行中降到 %5Mbps")
            .arg(w).arg(h).arg(fps).arg(highBps / 1e6).arg(lowBps / 1e6));
    say(QString());

    // ---------- A. 高码率基线 ----------
    lanrdp::H264Encoder enc;
    if (!enc.open(w, h, fps, highBps, 2)) {
        say(QStringLiteral("!! open(8M) 失败：%1").arg(enc.lastError()));
        return 3;
    }
    say(QStringLiteral("编码器：%1").arg(enc.encoderName()));

    Segment A;
    QString err;
    if (!runSegment(enc, w, h, 30, 0, true, A, err)) {
        say(QStringLiteral("!! 阶段A 失败：%1").arg(err));
        return 3;
    }
    summarize(A, fps);

    // ---------- B. 运行中降码率 ----------
    const bool setOk = enc.setBitrate(lowBps);
    Segment B;
    if (!runSegment(enc, w, h, 90, 30, false, B, err)) {
        say(QStringLiteral("!! 阶段B 失败：%1").arg(err));
        return 3;
    }
    summarize(B, fps);
    enc.close();

    // ---------- C. 干净低码率对照 ----------
    lanrdp::H264Encoder enc2;
    if (!enc2.open(w, h, fps, lowBps, 2)) {
        say(QStringLiteral("!! open(1M) 失败：%1").arg(enc2.lastError()));
        return 3;
    }
    Segment C;
    if (!runSegment(enc2, w, h, 30, 0, true, C, err)) {
        say(QStringLiteral("!! 阶段C 失败：%1").arg(err));
        return 3;
    }
    summarize(C, fps);
    enc2.close();

    // ---------- D. 极端低码率 open 对照（坐实 open 阶段码率是否生效） ----------
    const int extremeBps = 200 * 1000;
    lanrdp::H264Encoder enc3;
    if (!enc3.open(w, h, fps, extremeBps, 2)) {
        say(QStringLiteral("!! open(200k) 失败：%1").arg(enc3.lastError()));
        return 3;
    }
    Segment D;
    if (!runSegment(enc3, w, h, 30, 0, true, D, err)) {
        say(QStringLiteral("!! 阶段D 失败：%1").arg(err));
        return 3;
    }
    summarize(D, fps);
    enc3.close();

    // ---------- E. open(8M) 后立即 setBitrate(200k) 对照 ----------
    // 若 D 显示 open 阶段码率无效、而 E 能到 200k，则说明「运行中 setBitrate 是
    // 唯一有效的设码率通道」—— 自适应码率的实现必须在 open 后立刻 setBitrate 一次。
    lanrdp::H264Encoder enc4;
    if (!enc4.open(w, h, fps, highBps, 2)) {
        say(QStringLiteral("!! open(8M) 失败：%1").arg(enc4.lastError()));
        return 3;
    }
    const bool eSet = enc4.setBitrate(extremeBps);
    Segment E;
    if (!runSegment(enc4, w, h, 30, 0, true, E, err)) {
        say(QStringLiteral("!! 阶段E 失败：%1").arg(err));
        return 3;
    }
    summarize(E, fps);
    enc4.close();

    // ---------- F. open(8M) → 送 1 帧 → setBitrate(200k) → 送 29 帧 ----------
    // 验证「首帧后 setBitrate」是否已足够让码率生效（决定初始码率
    // 设置时序：open 阶段码率无效，必须等首帧后再设一次）。
    lanrdp::H264Encoder enc5;
    if (!enc5.open(w, h, fps, highBps, 2)) {
        say(QStringLiteral("!! open(8M) 失败：%1").arg(enc5.lastError()));
        return 3;
    }
    Segment fFirst, fRest;
    if (!runSegment(enc5, w, h, 1, 0, true, fFirst, err)) {
        say(QStringLiteral("!! 阶段F-首帧 失败：%1").arg(err));
        return 3;
    }
    const bool fSet = enc5.setBitrate(extremeBps);
    if (!runSegment(enc5, w, h, 29, 1, false, fRest, err)) {
        say(QStringLiteral("!! 阶段F-后续 失败：%1").arg(err));
        return 3;
    }
    Segment F;
    F.bytes.append(fFirst.bytes);
    F.bytes.append(fRest.bytes);
    F.keyframes.append(fFirst.keyframes);
    F.keyframes.append(fRest.keyframes);
    summarize(F, fps);
    enc5.close();

    // ---------- 输出 ----------
    say(QString());
    say(QStringLiteral("setBitrate(1M) 返回值：%1").arg(setOk ? QStringLiteral("true") : QStringLiteral("false")));
    say(QStringLiteral("setBitrate(200k) 返回值：%1").arg(eSet ? QStringLiteral("true") : QStringLiteral("false")));
    say(QStringLiteral("setBitrate(F阶段200k) 返回值：%1").arg(fSet ? QStringLiteral("true") : QStringLiteral("false")));
    say(QString());
    say(QStringLiteral("%1 %2 %3 %4 %5")
            .arg(QStringLiteral("阶段"), -14)
            .arg(QStringLiteral("帧数"), 6)
            .arg(QStringLiteral("平均字节/帧"), 14)
            .arg(QStringLiteral("折算Mbps"), 11)
            .arg(QStringLiteral("关键帧(均字节)"), 16));
    say(QStringLiteral("%1 %2 %3 %4 %5")
            .arg(QStringLiteral("A 高码率基线(8M)"), -14)
            .arg(A.bytes.size(), 6)
            .arg(num(A.avgBytes, 0), 14)
            .arg(num(A.avgMbps, 2), 11)
            .arg(QStringLiteral("%1 字节 x%2").arg(num(A.keyBytes, 0), 0).arg(A.keyCount)));
    say(QStringLiteral("%1 %2 %3 %4 %5")
            .arg(QStringLiteral("C 干净低码率(1M)"), -14)
            .arg(C.bytes.size(), 6)
            .arg(num(C.avgBytes, 0), 14)
            .arg(num(C.avgMbps, 2), 11)
            .arg(QStringLiteral("%1 字节 x%2").arg(num(C.keyBytes, 0), 0).arg(C.keyCount)));
    say(QStringLiteral("%1 %2 %3 %4 %5")
            .arg(QStringLiteral("D open(200k)"), -14)
            .arg(D.bytes.size(), 6)
            .arg(num(D.avgBytes, 0), 14)
            .arg(num(D.avgMbps, 2), 11)
            .arg(QStringLiteral("%1 字节 x%2").arg(num(D.keyBytes, 0), 0).arg(D.keyCount)));
    say(QStringLiteral("%1 %2 %3 %4 %5")
            .arg(QStringLiteral("E open(8M)+set(200k)"), -14)
            .arg(E.bytes.size(), 6)
            .arg(num(E.avgBytes, 0), 14)
            .arg(num(E.avgMbps, 2), 11)
            .arg(QStringLiteral("%1 字节 x%2").arg(num(E.keyBytes, 0), 0).arg(E.keyCount)));
    say(QStringLiteral("%1 %2 %3 %4 %5")
            .arg(QStringLiteral("F 8M+1帧+set(200k)"), -14)
            .arg(F.bytes.size(), 6)
            .arg(num(F.avgBytes, 0), 14)
            .arg(num(F.avgMbps, 2), 11)
            .arg(QStringLiteral("%1 字节 x%2").arg(num(F.keyBytes, 0), 0).arg(F.keyCount)));
    say(QString());

    // B 分桶：看降码率后第几帧收敛
    say(QStringLiteral("B 运行中降码率，按产出顺序分桶（fps=%1，GOP≈2s=%2帧）：").arg(fps).arg(fps * 2));
    say(QStringLiteral("%1 %2 %3")
            .arg(QStringLiteral("产出帧区间"), -12)
            .arg(QStringLiteral("平均字节/帧"), 14)
            .arg(QStringLiteral("折算Mbps"), 11));
    const int buckets[][2] = {{0, 5}, {5, 15}, {15, 30}, {30, 60}, {60, 90}};
    for (const auto &b : buckets) {
        double ab = 0, mb = 0;
        bucketAvg(B, b[0], b[1], fps, ab, mb);
        say(QStringLiteral("[%1-%2)    %3 %4")
                .arg(b[0], 3).arg(b[1], 3)
                .arg(num(ab, 0), 14)
                .arg(num(mb, 2), 11));
    }
    say(QString());

    // 结论判读
    double ab = 0, mb = 0;
    bucketAvg(B, 60, 90, fps, ab, mb);  // 稳态桶
    const double dropRatio = (A.avgBytes > 0) ? ab / A.avgBytes : 0;
    const double vsClean   = (C.avgBytes > 0) ? ab / C.avgBytes : 0;
    say(QStringLiteral("============ 判读 ============"));
    say(QStringLiteral("稳态桶[60-90) 折算 %1 Mbps；高码率基线 %2 Mbps → 降到 %3 %")
            .arg(num(mb, 2)).arg(num(A.avgMbps, 2)).arg(num(dropRatio * 100, 0)));
    say(QStringLiteral("稳态桶 vs 干净1M编码器 = %1x（≈1 说明运行中改码率能到目标码率）").arg(num(vsClean, 2)));
    say(QStringLiteral("关键帧字节：A=%1 / B=%2 / C=%3（关键帧最能反映码率上限是否真的降了）")
            .arg(num(A.keyBytes, 0)).arg(num(B.keyBytes, 0)).arg(num(C.keyBytes, 0)));

    os.flush();
    outFile.close();
    fprintf(stdout, "\n[done] -> %s\n", qPrintable(outPath));
    return 0;
}
