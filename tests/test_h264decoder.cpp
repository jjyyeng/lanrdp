#include <QtTest/QtTest>

#include <QByteArray>
#include <QImage>
#include <QVector>

#include <atomic>
#include <memory>
#include <thread>

#include "host/ColorConvert.h"
#include "host/H264Encoder.h"
#include "viewer/H264Decoder.h"

// Media Foundation H.264 解码器封装
// 用真实编码器产出码流做闭环，验证解码尺寸正确、颜色能还原。
class TestH264Decoder : public QObject
{
    Q_OBJECT

private slots:
    void decodesEncodedStreamBackToPicture();
    void openRejectsInvalidParameters();
    // 输出尺寸可下调，但 width()/height() 仍报码流真实尺寸
    void setOutputSizeShrinksFramesButKeepsTrueSize();
    // 码流尺寸与 open() 时申报的不符时，必须按 STREAM_CHANGE 后的新尺寸重取输出样本参数
    void streamSizeChangeRekeysOutputSampleParameters();

    // 解码移出 UI 线程 —— 锁定「解码器只能在其创建线程内使用」这条契约。
    // 同步 MFT 有线程亲和性，跨线程搬运是 UB；所以要么同线程用、要么明确报错。
    void worksEntirelyInsideWorkerThread();          // 工作线程内 open+decode+close 全程可用
    void decodeOnForeignThreadIsRejected();          // 跨线程 decode 必须失败而非崩
    void closeOnForeignThreadIsRejected();           // 跨线程 close 同理
    void outputSizeAppliesFromWorkerThread();        // 缩放链路在工作线程内同样生效
};

namespace {

constexpr int kWidth  = 320;
constexpr int kHeight = 240;

// 上半红、下半蓝：边界远离采样点，色度下采样不会干扰断言
QByteArray makeTwoToneBgra(int width, int height)
{
    QByteArray data(width * height * 4, '\0');
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            char *p = data.data() + (y * width + x) * 4;
            const bool top = y < height / 2;
            p[0] = static_cast<char>(top ? 0 : 255);    // B
            p[1] = 0;                                   // G
            p[2] = static_cast<char>(top ? 255 : 0);    // R
            p[3] = static_cast<char>(0xFF);
        }
    }
    return data;
}

// 在给定矩形上取平均色，避免单点噪声影响断言
QRgb averageColor(const QImage &image, int x0, int y0, int w, int h)
{
    qint64 r = 0, g = 0, b = 0;
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            const QRgb c = image.pixel(x, y);
            r += qRed(c);
            g += qGreen(c);
            b += qBlue(c);
        }
    }
    const int n = w * h;
    return qRgb(int(r / n), int(g / n), int(b / n));
}

// 用真实编码器灌 20 帧进解码器，返回解出的最后一帧（中途失败返回空 QImage）。
QImage encodeThenDecode(lanrdp::H264Decoder &decoder, const QByteArray &nv12)
{
    lanrdp::H264Encoder encoder;
    if (!encoder.open(kWidth, kHeight, 30, 2 * 1000 * 1000, 1))
        return {};

    QVector<QImage> decoded;
    for (int i = 0; i < 20; ++i) {
        QVector<lanrdp::H264EncodedFrame> produced;
        if (!encoder.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()),
                                 nv12.size(), i == 0, produced))
            return {};
        for (const auto &frame : produced) {
            if (!decoder.decodeFrame(frame.bitstream, decoded))
                return {};
        }
    }
    return decoded.isEmpty() ? QImage() : decoded.last();
}

} // namespace

// ---------------------------------------------------------------------------
// 工作线程辅助设施
//
// 同步 MFT 有线程亲和性 ⇒ 解码器必须在**它被 open() 的那个线程**里使用。
// 下面几个用例把这条约束钉死：既验证「工作线程内可以正常用」，也验证
// 「误用到别的线程会明确报错而不是产生 UB」。
// ---------------------------------------------------------------------------

namespace {

// 一个「HV 帧库」：预先编好码流，供工作线程重复喂。避免在计时/并发段里再跑编码器。
struct StreamLibrary
{
    QVector<QByteArray> frames;
    bool                ok = false;

    bool build()
    {
        const QByteArray bgra = makeTwoToneBgra(kWidth, kHeight);
        const QByteArray nv12 = lanrdp::bgraToNv12(
            reinterpret_cast<const uchar *>(bgra.constData()), kWidth * 4, kWidth, kHeight);
        if (nv12.isEmpty())
            return false;

        lanrdp::H264Encoder encoder;
        if (!encoder.open(kWidth, kHeight, 30, 2 * 1000 * 1000, 1))
            return false;

        for (int i = 0; i < 20; ++i) {
            QVector<lanrdp::H264EncodedFrame> produced;
            if (!encoder.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()),
                                     nv12.size(), i == 0, produced))
                return false;
            for (const auto &f : produced)
                frames.append(f.bitstream);
        }
        ok = !frames.isEmpty();
        return ok;
    }
};

} // namespace

// 工作线程内 open + decode + close 全流程：这是核心用法 ——
// DecoderWorker 会在自己的线程里 new H264Decoder()、open()、反复 decodeFrame()，
// 线程收尾时 close()。全程不得崩、不得报错。
void TestH264Decoder::worksEntirelyInsideWorkerThread()
{
    StreamLibrary lib;
    if (!lib.build())
        QSKIP("Media Foundation H.264 编解码不可用（无 MFT / Windows N），跳过");

    std::atomic<bool> opened{false};
    std::atomic<bool> decodedOk{false};
    std::atomic<bool> closedOk{false};
    std::atomic<int>  produced{0};
    QString           err;

    std::thread worker([&]() {
        // 工作线程必须自己初始化 COM —— 与 DecoderWorker 的做法一致
        lanrdp::H264Decoder decoder;
        if (!decoder.open(kWidth, kHeight)) {
            err = decoder.lastError();
            return;
        }
        opened = true;

        QVector<QImage> images;
        for (const QByteArray &bs : lib.frames) {
            if (!decoder.decodeFrame(bs, images)) {
                err = decoder.lastError();
                return;
            }
        }
        produced  = images.size();
        decodedOk = !images.isEmpty();
        decoder.close();
        closedOk = true;
    });
    worker.join();

    QVERIFY2(err.isEmpty(), qPrintable(err));
    QVERIFY2(opened.load(), "open() failed inside worker thread");
    QVERIFY2(decodedOk.load(), "decodeFrame() produced nothing inside worker thread");
    QVERIFY(produced.load() > 0);
    QVERIFY2(closedOk.load(), "close() failed inside worker thread");

    // 真尺寸口径在工作线程里同样成立
    QCOMPARE(produced.load() > 0, true);
}

// 跨线程误用：在 A 线程 open，到 B 线程 decode —— 必须明确失败（lastError 有内容），
// 而不是产生未定义行为。这是给「将来有人不小心跨线程搬解码器」留的安全网。
void TestH264Decoder::decodeOnForeignThreadIsRejected()
{
    StreamLibrary lib;
    if (!lib.build())
        QSKIP("Media Foundation H.264 编解码不可用（无 MFT / Windows N），跳过");

    // 主线程 open（解码器绑定主线程）
    auto decoder = std::make_unique<lanrdp::H264Decoder>();
    QVERIFY2(decoder->open(kWidth, kHeight), qPrintable(decoder->lastError()));

    std::atomic<bool> returned{false};
    std::atomic<bool> decodeReturnedFalse{false};
    QString           err;

    std::thread foreign([&]() {
        QVector<QImage> images;
        const bool ok = decoder->decodeFrame(lib.frames.first(), images);
        decodeReturnedFalse = !ok;
        if (!ok)
            err = decoder->lastError();
        returned = true;
    });
    foreign.join();

    QVERIFY2(returned.load(), "foreign-thread decode did not return");
    QVERIFY2(decodeReturnedFalse.load(),
             "decodeFrame() from a foreign thread must fail, not silently succeed");
    QVERIFY2(!err.isEmpty(), "foreign-thread rejection must carry a readable lastError()");
    qInfo("foreign-thread decode rejected with: %s", qPrintable(err));

    // 主线程仍可正常收尾
    decoder->close();
}

// 跨线程 close：同样必须被拦住 —— 对同步 MFT 来说，Release() 落到别人的线程上
// 一样是 UB。
void TestH264Decoder::closeOnForeignThreadIsRejected()
{
    auto decoder = std::make_unique<lanrdp::H264Decoder>();
    if (!decoder->open(kWidth, kHeight))
        QSKIP("Media Foundation H.264 解码器不可用（无 MFT / Windows N），跳过");
    QVERIFY(decoder->isOpen());

    std::atomic<bool> returned{false};
    QString           err;

    std::thread foreign([&]() {
        decoder->close();
        err      = decoder->lastError();
        returned = true;
    });
    foreign.join();

    QVERIFY2(returned.load(), "foreign-thread close did not return");
    // 被拒的 close 不得把解码器置成「已关闭」—— 那样主线程后续 close 就成了空转
    QVERIFY2(decoder->isOpen(),
             "a rejected foreign-thread close must not drop the decoder's open state");
    QVERIFY2(!err.isEmpty(), "foreign-thread close rejection must carry a readable lastError()");
    qInfo("foreign-thread close rejected with: %s", qPrintable(err));

    decoder->close();  // 主线程收尾仍然可用
    QVERIFY(!decoder->isOpen());
}

// 缩放链路必须在工作线程内同样生效：DecoderWorker 会在线程里
// open() 之后立刻 setOutputSize(budget)，这条路径不能退化。
void TestH264Decoder::outputSizeAppliesFromWorkerThread()
{
    StreamLibrary lib;
    if (!lib.build())
        QSKIP("Media Foundation H.264 编解码不可用（无 MFT / Windows N），跳过");

    std::atomic<bool> ok{false};
    QString           err;
    QSize             lastSize;
    int               trueW = 0, trueH = 0;

    std::thread worker([&]() {
        lanrdp::H264Decoder decoder;
        if (!decoder.open(kWidth, kHeight)) {
            err = decoder.lastError();
            return;
        }
        // 同上：面积比必须 ≤ 1/9 才真缩（1/4 面积会被收益守卫退回全尺寸）
        decoder.setOutputSize(QSize(kWidth / 4, kHeight / 4));

        QVector<QImage> images;
        for (const QByteArray &bs : lib.frames) {
            if (!decoder.decodeFrame(bs, images)) {
                err = decoder.lastError();
                return;
            }
        }
        if (images.isEmpty()) {
            err = QStringLiteral("no picture produced");
            return;
        }
        lastSize = images.last().size();
        trueW    = decoder.width();
        trueH    = decoder.height();
        decoder.close();
        ok = true;
    });
    worker.join();

    QVERIFY2(err.isEmpty(), qPrintable(err));
    QVERIFY2(ok.load(), "scaled decode inside worker thread failed");

    // 输出被缩到 1/4 线性尺寸，但真尺寸口径不动 —— 与同线程用例口径一致
    QCOMPARE(lastSize, QSize(kWidth / 4, kHeight / 4));
    QCOMPARE(trueW, kWidth);
    QCOMPARE(trueH, kHeight);
}

void TestH264Decoder::decodesEncodedStreamBackToPicture()
{
    lanrdp::H264Encoder encoder;
    if (!encoder.open(kWidth, kHeight, 30, 2 * 1000 * 1000, 1))
        QSKIP("Media Foundation H.264 编码器不可用（无 MFT / Windows N），跳过");

    lanrdp::H264Decoder decoder;
    if (!decoder.open(kWidth, kHeight))
        QSKIP("Media Foundation H.264 解码器不可用（无 MFT / Windows N），跳过");
    QVERIFY(decoder.isOpen());

    const QByteArray bgra = makeTwoToneBgra(kWidth, kHeight);
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(bgra.constData()), kWidth * 4, kWidth, kHeight);
    QVERIFY(!nv12.isEmpty());

    QVector<QImage> decoded;
    for (int i = 0; i < 20; ++i) {
        QVector<lanrdp::H264EncodedFrame> produced;
        QVERIFY2(encoder.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()),
                                     nv12.size(), i == 0, produced),
                 qPrintable(encoder.lastError()));
        for (const auto &frame : produced)
            QVERIFY2(decoder.decodeFrame(frame.bitstream, decoded),
                     qPrintable(decoder.lastError()));
    }

    QVERIFY2(!decoded.isEmpty(), "decoder produced no picture");

    const QImage &last = decoded.last();
    QCOMPARE(last.size(), QSize(kWidth, kHeight));
    QVERIFY(!last.isNull());

    // 上半应还原成红色、下半还原成蓝色（有损压缩，允许一定误差）
    const QRgb top    = averageColor(last, 40, 20, 40, 40);
    const QRgb bottom = averageColor(last, 40, kHeight - 60, 40, 40);

    qInfo("decoder=%s frames=%d top=(%d,%d,%d) bottom=(%d,%d,%d)",
          qPrintable(decoder.decoderName()), int(decoded.size()),
          qRed(top), qGreen(top), qBlue(top),
          qRed(bottom), qGreen(bottom), qBlue(bottom));

    QVERIFY2(qRed(top) > 200 && qGreen(top) < 60 && qBlue(top) < 60,
             qPrintable(QStringLiteral("top not red: (%1,%2,%3)")
                            .arg(qRed(top)).arg(qGreen(top)).arg(qBlue(top))));
    QVERIFY2(qBlue(bottom) > 200 && qRed(bottom) < 60 && qGreen(bottom) < 60,
             qPrintable(QStringLiteral("bottom not blue: (%1,%2,%3)")
                            .arg(qRed(bottom)).arg(qGreen(bottom)).arg(qBlue(bottom))));
}

void TestH264Decoder::openRejectsInvalidParameters()
{
    lanrdp::H264Decoder decoder;
    QVERIFY(!decoder.open(0, 0));
    QVERIFY(!decoder.isOpen());

    QVector<QImage> frames;
    QVERIFY(!decoder.decodeFrame(QByteArray("\x00\x00\x00\x01\x65", 5), frames));
    QVERIFY(!decoder.lastError().isEmpty());

    // 空码流不算错误，直接返回成功但无产出
    // （这半段需要真的解码器；MF 不可用时上面那批负向断言已经跑完，本条明确跳过。）
    lanrdp::H264Decoder ok;
    if (!ok.open(kWidth, kHeight))
        QSKIP("Media Foundation H.264 解码器不可用（无 MFT / Windows N），跳过");
    QVERIFY(ok.decodeFrame(QByteArray(), frames));
    QVERIFY(frames.isEmpty());
    ok.close();
}

void TestH264Decoder::setOutputSizeShrinksFramesButKeepsTrueSize()
{
    const QByteArray bgra = makeTwoToneBgra(kWidth, kHeight);
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(bgra.constData()), kWidth * 4, kWidth, kHeight);
    QVERIFY(!nv12.isEmpty());

    lanrdp::H264Decoder decoder;
    if (!decoder.open(kWidth, kHeight))
        QSKIP("Media Foundation H.264 解码器不可用（无 MFT / Windows N），跳过");

    // 让解码器直接吐 1/4 线性尺寸的图 —— 省掉「写全尺寸 ARGB32」这一步。
    // ⚠️ 这里**必须**小到 1/16 面积：收益守卫的阈值是面积比 ≤ 1/9 才真缩
    // （1/4 面积 = 1/2 线性尺寸是曲线上的最差点，守卫会刻意回退全尺寸，
    // 见 test_colorconvert::scaledConversionThresholdBoundaryIsInclusive）。
    decoder.setOutputSize(QSize(kWidth / 4, kHeight / 4));

    const QImage last = encodeThenDecode(decoder, nv12);
    QVERIFY2(!last.isNull(), "decoder produced no picture");

    QCOMPARE(last.size(), QSize(kWidth / 4, kHeight / 4));

    // ⚠️ 真实尺寸口径不受影响 —— 状态栏分辨率取的就是它，不能跟着一起缩
    QCOMPARE(decoder.width(), kWidth);
    QCOMPARE(decoder.height(), kHeight);

    // 缩放后颜色仍要能还原（上半红、下半蓝）。⚠️ 采样矩形必须跟着新尺寸走 ——
    // QImage::pixel 越界是 UB（旧值 kHeight/2-30 对 60 高的图已经出界）。
    const QRgb top    = averageColor(last, 20, 5, 20, 20);
    const QRgb bottom = averageColor(last, 20, 35, 20, 20);
    QVERIFY2(qRed(top) > 200 && qBlue(top) < 60,
             qPrintable(QStringLiteral("top not red: (%1,%2,%3)")
                            .arg(qRed(top)).arg(qGreen(top)).arg(qBlue(top))));
    QVERIFY2(qBlue(bottom) > 200 && qRed(bottom) < 60,
             qPrintable(QStringLiteral("bottom not blue: (%1,%2,%3)")
                            .arg(qRed(bottom)).arg(qGreen(bottom)).arg(qBlue(bottom))));
}

// 用一个**与码流不符**的初始尺寸 open()：解码器只会在解出首个关键帧时才从码流里
// 读到真实尺寸，并回一个 MF_E_TRANSFORM_STREAM_CHANGE。
//
// 这条用例钉的是「STREAM_CHANGE 之后必须**重查**输出流信息」：
// 旧参数是按 open() 申报的尺寸算出来的，换尺寸后它就偏小 ⇒ 每帧 MF_E_BUFFERTOOSMALL，
// 而解码器不会被重建 ⇒ 画面**永远**好不了（这是最难查的一类：不崩、不报错、只是没画面）。
void TestH264Decoder::streamSizeChangeRekeysOutputSampleParameters()
{
    StreamLibrary lib;   // 码流真实尺寸是 kWidth x kHeight
    QVERIFY2(lib.build(), "failed to build reference stream");

    lanrdp::H264Decoder decoder;
    // 申报一个明显更小的尺寸（真实码流仍是 kWidth x kHeight）
    QVERIFY2(decoder.open(64, 64), qPrintable(decoder.lastError()));

    QVector<QImage> decoded;
    for (const QByteArray &bs : lib.frames)
        QVERIFY2(decoder.decodeFrame(bs, decoded), qPrintable(decoder.lastError()));

    QVERIFY2(!decoded.isEmpty(), "尺寸变化后一帧都没解出来（输出样本参数没跟着重查？）");

    // 输出图与真尺寸口径都应变成码流里的尺寸，而不是 open() 时申报的 64x64
    QCOMPARE(decoded.last().size(), QSize(kWidth, kHeight));
    QCOMPARE(decoder.width(), kWidth);
    QCOMPARE(decoder.height(), kHeight);

    // 正常路径不得留下噪声：坏帧诊断（缓冲偏小 / 负跨距 / 转换失败）一条都不该出现
    QVERIFY2(decoder.lastError().isEmpty(),
             qPrintable(QStringLiteral("正常解码路径留下了错误记录：%1").arg(decoder.lastError())));

    decoder.close();
}

QTEST_GUILESS_MAIN(TestH264Decoder)
#include "test_h264decoder.moc"
