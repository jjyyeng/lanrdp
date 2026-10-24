#include <QtTest/QtTest>

#include <QByteArray>
#include <QFile>
#include <QVector>

#include "host/ColorConvert.h"
#include "host/H264Encoder.h"

// Media Foundation H.264 编码器封装
// 用合成画面跑真实编码，验证产出的是带起始码的 Annex-B 码流且首帧带参数集。
class TestH264Encoder : public QObject
{
    Q_OBJECT

private slots:
    void encodesFramesIntoAnnexBWithParameterSets();
    void forcedKeyframeProducesSelfContainedIdr();
    void rejectsMismatchedFrameSize();
    void openRejectsInvalidParameters();
    void normalizeRejectsHugeNalSizeWithoutOverflow();
    void normalizeConvertsAvccToAnnexB();
    void normalizeKeepsAnnexBAsIs();
    // ICodecAPI 是可选接口：必须如实报告「有没有」，失败要留可读原因，不许静默
    void bitrateControlIsReportedNotSilentlyDropped();
};

namespace {

constexpr int kWidth  = 320;
constexpr int kHeight = 240;

// 生成一帧会缓慢变化的 BGRA 画面（避免纯色导致编码器产出过小、难以判断）
QByteArray makeBgraFrame(int width, int height, int tick)
{
    QByteArray data(width * height * 4, '\0');
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            char *p = data.data() + (y * width + x) * 4;
            p[0] = static_cast<char>((x + tick * 3) & 0xFF);   // B
            p[1] = static_cast<char>((y + tick * 5) & 0xFF);   // G
            p[2] = static_cast<char>((x + y + tick) & 0xFF);   // R
            p[3] = static_cast<char>(0xFF);
        }
    }
    return data;
}

bool startsWithStartCode(const QByteArray &data)
{
    if (data.size() < 4)
        return false;
    return static_cast<quint8>(data.at(0)) == 0 && static_cast<quint8>(data.at(1)) == 0
           && static_cast<quint8>(data.at(2)) == 0 && static_cast<quint8>(data.at(3)) == 1;
}

// 在 Annex-B 码流里找指定 NAL 类型（低 5 位）
bool containsNalType(const QByteArray &data, int nalType)
{
    for (int i = 0; i + 4 < data.size(); ++i) {
        if (static_cast<quint8>(data.at(i)) == 0 && static_cast<quint8>(data.at(i + 1)) == 0
            && static_cast<quint8>(data.at(i + 2)) == 0 && static_cast<quint8>(data.at(i + 3)) == 1) {
            const int header = static_cast<quint8>(data.at(i + 4));
            if ((header & 0x1F) == nalType)
                return true;
        }
    }
    return false;
}

} // namespace

void TestH264Encoder::encodesFramesIntoAnnexBWithParameterSets()
{
    lanrdp::H264Encoder encoder;
    const int           bitrate = 2 * 1000 * 1000;
    // 环境不具备（无 MFT / Windows N / Server Core）时明确跳过 ——
    // 这类机器上「编码器打不开」与「封装的错」无关，报成红会掩盖真实回归。
    if (!encoder.open(kWidth, kHeight, 30, bitrate, 1))
        QSKIP("Media Foundation H.264 编码器不可用（无 MFT / Windows N），跳过");
    QVERIFY(encoder.isOpen());

    QVector<lanrdp::H264EncodedFrame> outputs;
    for (int tick = 0; tick < 40; ++tick) {
        const QByteArray bgra = makeBgraFrame(kWidth, kHeight, tick);
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(bgra.constData()), kWidth * 4, kWidth, kHeight);
        QCOMPARE(nv12.size(), kWidth * kHeight * 3 / 2);

        QVector<lanrdp::H264EncodedFrame> produced;
        QVERIFY2(encoder.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()),
                                     nv12.size(), tick == 0, produced),
                 qPrintable(encoder.lastError()));
        outputs += produced;
    }

    QVERIFY2(!outputs.isEmpty(), "encoder produced no output at all");

    qint64 totalBytes = 0;
    for (const auto &frame : outputs)
        totalBytes += frame.bitstream.size();
    QVERIFY(totalBytes > 0);

    // 首帧必须是关键帧，且带起始码 + SPS/PPS
    const lanrdp::H264EncodedFrame &first = outputs.first();
    QVERIFY2(first.keyframe, "first output frame is not a keyframe");
    QVERIFY2(startsWithStartCode(first.bitstream), "bitstream is not Annex-B (no start code)");
    QVERIFY2(containsNalType(first.bitstream, 7), "no SPS (NAL type 7) in first frame");
    QVERIFY2(containsNalType(first.bitstream, 8), "no PPS (NAL type 8) in first frame");

    // 每帧都应是 Annex-B
    for (const auto &frame : outputs)
        QVERIFY(startsWithStartCode(frame.bitstream));

    qInfo("encoder=%s frames=%d bytes=%lld",
          qPrintable(encoder.encoderName()), int(outputs.size()), totalBytes);

    // 落盘一份码流，便于人工/后续解码器测试复用
    QFile dump(QStringLiteral("h264_sample.264"));
    if (dump.open(QIODevice::WriteOnly)) {
        for (const auto &frame : outputs)
            dump.write(frame.bitstream);
        dump.close();
    }

    encoder.close();
    QVERIFY(!encoder.isOpen());
}

void TestH264Encoder::forcedKeyframeProducesSelfContainedIdr()
{
    // 观看者中途加入时，解码器手里没有任何参考帧，必须马上收到一个自带 SPS/PPS 的 IDR。
    // 微软编码器不会为「新连接」自动出关键帧，所以靠 CODECAPI_AVEncVideoForceKeyFrame 主动索取。
    lanrdp::H264Encoder encoder;
    if (!encoder.open(kWidth, kHeight, 10, 2 * 1000 * 1000, /*gopSeconds=*/0))
        QSKIP("Media Foundation H.264 编码器不可用（无 MFT / Windows N），跳过");

    QVector<lanrdp::H264EncodedFrame> outputs;

    auto feed = [&](int tick, bool force) {
        const QByteArray bgra = makeBgraFrame(kWidth, kHeight, tick);
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(bgra.constData()), kWidth * 4, kWidth, kHeight);
        QVector<lanrdp::H264EncodedFrame> produced;
        const bool ok = encoder.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()),
                                            nv12.size(), force, produced);
        QVERIFY2(ok, qPrintable(encoder.lastError()));
        outputs += produced;
    };

    auto countKeyframes = [](const QVector<lanrdp::H264EncodedFrame> &frames) {
        int n = 0;
        for (const auto &f : frames)
            if (f.keyframe)
                ++n;
        return n;
    };

    for (int i = 0; i < 10; ++i)
        feed(i, /*force=*/false);

    const int before = countKeyframes(outputs);
    QCOMPARE(before, 1);   // 只有首帧是关键的

    // 强制要一个关键帧，再多喂几帧把流水线冲出来
    feed(10, /*force=*/true);
    for (int i = 11; i < 22; ++i)
        feed(i, false);

    const int after = countKeyframes(outputs);
    QVERIFY2(after > before,
             "CODECAPI_AVEncVideoForceKeyFrame 没生效：强制之后没有出现新的关键帧");

    // 关键帧必须是自包含的——否则中途加入的观看者拿到也不能解码
    for (const auto &f : outputs) {
        if (!f.keyframe)
            continue;
        QVERIFY2(containsNalType(f.bitstream, 7), "keyframe missing SPS (NAL 7)");
        QVERIFY2(containsNalType(f.bitstream, 8), "keyframe missing PPS (NAL 8)");
    }

    qInfo("total=%d keyframes: before=%d after=%d", int(outputs.size()), before, after);
    encoder.close();
}

void TestH264Encoder::rejectsMismatchedFrameSize()
{
    lanrdp::H264Encoder encoder;
    if (!encoder.open(kWidth, kHeight, 30, 1000 * 1000))
        QSKIP("Media Foundation H.264 编码器不可用（无 MFT / Windows N），跳过");

    const QByteArray bgra = makeBgraFrame(kWidth, kHeight, 0);
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(bgra.constData()), kWidth * 4, kWidth, kHeight);

    QVector<lanrdp::H264EncodedFrame> produced;
    // 字节数对不上必须被拒绝，而不是读到越界内存
    QVERIFY(!encoder.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()),
                                 nv12.size() - 1, false, produced));
    QVERIFY(!encoder.lastError().isEmpty());

    encoder.close();
}

void TestH264Encoder::openRejectsInvalidParameters()
{
    lanrdp::H264Encoder encoder;
    QVERIFY(!encoder.open(0, 0, 30, 1000 * 1000));
    QVERIFY(!encoder.open(321, 240, 30, 1000 * 1000));   // 奇数宽，NV12 不支持
    QVERIFY(!encoder.open(kWidth, kHeight, 0, 1000 * 1000));
    QVERIFY(!encoder.isOpen());

    // 未打开时编码应失败而不是崩溃
    QVector<lanrdp::H264EncodedFrame> produced;
    QVERIFY(!encoder.encodeFrame(reinterpret_cast<const uchar *>("x"), 1, false, produced));
}

// nalSize 取自线上 4 字节（最大 0x7FFFFFFF）。若边界检查写成 `offset + nalSize > size`，
// 加法会**有符号溢出成负数**，检查被绕过 ⇒ 越界读。这里构造「超大 nalSize」载荷，
// 断言必须原样返回（判为解析失败），绝不能按那个尺寸去 append。
void TestH264Encoder::normalizeRejectsHugeNalSizeWithoutOverflow()
{
    QByteArray payload;
    payload.append(char(0x7F));     // 大端 0x7FFFFFFF：一个天文数字的 NALU 长度
    payload.append(char(0xFF));
    payload.append(char(0xFF));
    payload.append(char(0xFF));
    payload.append("payload-body", 12);

    const QByteArray out = lanrdp::normalizeToAnnexB(payload);
    // 解析失败即原样返回；关键是**没有崩、也没有按那尺寸读出越界内存**
    QCOMPARE(out, payload);
}

// AVCC（每个 NALU 前置 4 字节大端长度）应被转成 Annex-B（4 字节起始码）。
void TestH264Encoder::normalizeConvertsAvccToAnnexB()
{
    QByteArray avcc;
    // 第一个 NALU：长度 3 + 三个字节
    avcc.append(char(0x00));
    avcc.append(char(0x00));
    avcc.append(char(0x00));
    avcc.append(char(0x03));
    avcc.append("abc", 3);
    // 第二个 NALU：长度 2 + 两个字节
    avcc.append(char(0x00));
    avcc.append(char(0x00));
    avcc.append(char(0x00));
    avcc.append(char(0x02));
    avcc.append("de", 2);

    const QByteArray out = lanrdp::normalizeToAnnexB(avcc);
    // 用 sizeof-1 而不是手写长度：字符串字面量里的 \x00 是**数据**，
    // 一旦长度写多就会越界读到字面量之外（本来会变成一条假失败）。
    static const char kExpected[] = "\x00\x00\x00\x01" "abc" "\x00\x00\x00\x01" "de";
    QCOMPARE(out, QByteArray(kExpected, sizeof(kExpected) - 1));
}

// 已经是 Annex-B（开头就有起始码）时应原样返回，不做二次转换。
void TestH264Encoder::normalizeKeepsAnnexBAsIs()
{
    static const char kAnnexB[] = "\x00\x00\x00\x01" "nope";
    const QByteArray in(kAnnexB, 8);
    QCOMPARE(lanrdp::normalizeToAnnexB(in), in);
}

// `ICodecAPI` 是**可选**接口，不是每个同步 H.264 MFT 都有。拿不到时：
//   · setBitrate 一定失败 —— 关键是**必须留可读原因**（从前是静默 return false，
//     lastError 为空，上层无从判断「为什么码率设了没效果」）；
//   · encodeFrame(forceKeyframe) 没法强制 IDR —— 新观看者要等下一个自然关键帧。
// 这条钉住「如实报告」这个契约，而不是钉死「本机一定有」。
void TestH264Encoder::bitrateControlIsReportedNotSilentlyDropped()
{
    lanrdp::H264Encoder encoder;

    // 打开之前不该声称有能力（m_impl 还没建/没查出 ICodecAPI）
    QVERIFY2(!encoder.bitrateControlAvailable(), "未打开时不该声称码率可控");

    if (!encoder.open(kWidth, kHeight, 30, 2 * 1000 * 1000, 1))
        QSKIP("Media Foundation H.264 编码器不可用（无 MFT / Windows N），跳过");

    if (encoder.bitrateControlAvailable()) {
        // 有 ICodecAPI：改码率应当成功（微软同步编码器属于这一档）
        QVERIFY2(encoder.setBitrate(1000 * 1000), qPrintable(encoder.lastError()));
    } else {
        QVERIFY2(!encoder.setBitrate(1000 * 1000), "没有 ICodecAPI 时 setBitrate 必须失败");
        QVERIFY2(!encoder.lastError().isEmpty(), "失败必须给出原因，不能静默吞掉");
    }

    // 强制关键帧：没有 ICodecAPI 时必须留痕（encodeFrame 本身仍可成功产出普通帧）
    {
        const QByteArray bgra = makeBgraFrame(kWidth, kHeight, 0);
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(bgra.constData()), kWidth * 4, kWidth, kHeight);
        QVector<lanrdp::H264EncodedFrame> produced;
        QVERIFY2(encoder.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()),
                                     nv12.size(), /*forceKeyframe=*/true, produced),
                 qPrintable(encoder.lastError()));
        if (!encoder.bitrateControlAvailable())
            QVERIFY2(encoder.lastError().contains(QStringLiteral("关键帧")),
                     qPrintable(QStringLiteral("强制关键帧没生效时必须说明原因，实际：%1")
                                    .arg(encoder.lastError())));
    }

    encoder.close();
    QVERIFY2(!encoder.bitrateControlAvailable(), "关闭后不该还声称码率可控");
}

QTEST_GUILESS_MAIN(TestH264Encoder)
#include "test_h264encoder.moc"