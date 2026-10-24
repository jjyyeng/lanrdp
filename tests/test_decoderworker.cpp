#include <QtTest/QtTest>

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QSignalSpy>
#include <QVector>

#include <atomic>
#include <functional>
#include <thread>

#include "host/ColorConvert.h"
#include "host/H264Encoder.h"
#include "viewer/DecoderWorker.h"
#include "viewer/H264Decoder.h"

// H.264 解码移出 UI 线程 —— DecoderWorker 的契约测试。
//
// 这个文件要钉死的核心事实是：**解码不在调用线程上执行**。
// 光看「能解出画面」不够 —— 实现里把解码放回 UI 线程照样能出画面，
// 所以必须直接观测「产出帧的那个线程不是主线程」。
class TestDecoderWorker : public QObject
{
    Q_OBJECT

private slots:
    void decodesOnItsOwnThreadNotCallingThread();
    void outputSizeAppliesFromWorkerThread();
    void outputSizeChangeMidStreamTakesEffect();
    void reportsOpenFailureForInvalidSize();
    void stopJoinsThreadCleanly();
    void queuedFramesArriveInOrder();
    void readyTimeoutLeavesStateForRealResult();
};

namespace {

constexpr int kWidth  = 320;
constexpr int kHeight = 240;

// 上半红、下半蓝（与 test_h264decoder 同款，边界远离采样点）
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

// 预编好的 Annex-B 码流库 —— 避免在并发/计时段里再跑编码器
struct StreamLibrary
{
    QVector<QByteArray> frames;

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
        return !frames.isEmpty();
    }
};

// 泵事件直到满足条件或超时（worker 的 frameDecoded 是队列投递到本线程的）
bool pumpUntil(const std::function<bool()> &done, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

// 把整库码流喂给 worker（decode 内部是线程安全入队），等收到至少一帧
bool feedAll(lanrdp::DecoderWorker *worker, const StreamLibrary &lib, QSignalSpy &spy)
{
    for (const QByteArray &bs : lib.frames)
        worker->decode(bs);
    return pumpUntil([&spy]() { return spy.count() > 0; });
}

} // namespace

// ⭐ 本文件最重要的一条：证明**解码产帧发生在别的线程上**。
//
// 做法：用 Qt::DirectConnection 连 frameDecoded —— 直连时槽在**发射者所在线程**
// 执行（即工作线程），于是可以在这里读到那根线程的 id。队列连接做不到这点
// （槽会回到主线程执行），所以这里必须直连。
void TestDecoderWorker::decodesOnItsOwnThreadNotCallingThread()
{
    StreamLibrary lib;
    if (!lib.build())
        QSKIP("Media Foundation H.264 编解码不可用（无 MFT / Windows N），跳过");

    const std::thread::id mainId = std::this_thread::get_id();

    auto *worker = new lanrdp::DecoderWorker(kWidth, kHeight, this);
    worker->start();
    QVERIFY2(worker->waitReady(), qPrintable(worker->lastError()));
    QCOMPARE(worker->width(), kWidth);
    QCOMPARE(worker->height(), kHeight);
    QVERIFY2(worker->decoderName().contains(QStringLiteral("H264")),
             qPrintable(worker->decoderName()));

    // 直连：槽在工作线程执行，正好用来取证
    // ⚠️ 直连槽在**工作线程**执行，因此槽里写、主线程读的量**必须**是原子的 ——
    // 早先用非原子局部量是实打实的数据竞争（且是 UB）。尺寸拆成两个 int，线程 id
    // 折成「是否不等于主线程」的 bool（std::thread::id 本身不能原子化）。
    std::atomic<int>  directEmits{0};
    std::atomic<bool> emittedOffMainThread{false};
    std::atomic<int>  lastW{0};
    std::atomic<int>  lastH{0};
    std::atomic<int>  lastSrcW{0};
    std::atomic<int>  lastSrcH{0};
    connect(worker, &lanrdp::DecoderWorker::frameDecoded, this,
            [&](const QImage &image, const QSize &sourceSize) {
                if (std::this_thread::get_id() != mainId)
                    emittedOffMainThread.store(true, std::memory_order_relaxed);
                lastW.store(image.width(), std::memory_order_relaxed);
                lastH.store(image.height(), std::memory_order_relaxed);
                lastSrcW.store(sourceSize.width(), std::memory_order_relaxed);
                lastSrcH.store(sourceSize.height(), std::memory_order_relaxed);
                directEmits.fetch_add(1);
            },
            Qt::DirectConnection);

    QSignalSpy spy(worker, &lanrdp::DecoderWorker::frameDecoded);
    QVERIFY2(feedAll(worker, lib, spy), "no frame delivered");

    QVERIFY2(directEmits.load() > 0, "no frame was emitted");

    // ⭐ 核心断言：产帧线程 ≠ 主线程 —— 这就是「解码已移出 UI 线程」的直接证据
    QVERIFY2(emittedOffMainThread.load(),
             "frames were produced on the CALLING thread — decoding is still on the UI thread");

    QCOMPARE(worker->width(), kWidth);
    QCOMPARE(worker->height(), kHeight);
    QCOMPARE(QSize(lastSrcW.load(), lastSrcH.load()), QSize(kWidth, kHeight));
    QVERIFY(lastW.load() > 0 && lastH.load() > 0);

    qInfo("decoded %d frames on a worker thread (distinct from the UI thread)",
          directEmits.load());

    worker->stop();
    QVERIFY(!worker->isRunning());
}

// 显示预算链路在搬到工作线程后不能退化：
// 输出被缩到 1/2，但 width()/height() 仍报码流真实尺寸。
// 而且这里刻意**在 start() 之前**设预算 —— 验证 run() 里「建好解码器就补发」那条惰性路径。
void TestDecoderWorker::outputSizeAppliesFromWorkerThread()
{
    StreamLibrary lib;
    if (!lib.build())
        QSKIP("Media Foundation H.264 编解码不可用（无 MFT / Windows N），跳过");

    auto *worker = new lanrdp::DecoderWorker(kWidth, kHeight, this);
    // ⚠️ 面积比要 ≤ 1/9（收益守卫的阈值）才真缩；1/4 面积（= kWidth/2）会被回退成全尺寸，
    // 见 test_colorconvert::scaledConversionThresholdBoundaryIsInclusive
    worker->setOutputSize(QSize(kWidth / 4, kHeight / 4));  // 线程还没起，先记着
    worker->start();
    QVERIFY2(worker->waitReady(), qPrintable(worker->lastError()));

    QSignalSpy spy(worker, &lanrdp::DecoderWorker::frameDecoded);
    QVERIFY2(feedAll(worker, lib, spy), "no frame delivered");

    const QList<QVariant> args = spy.last();
    QCOMPARE(args.size(), 2);
    const QImage image = args.at(0).value<QImage>();
    const QSize  src   = args.at(1).value<QSize>();

    QCOMPARE(image.size(), QSize(kWidth / 4, kHeight / 4));
    QCOMPARE(src, QSize(kWidth, kHeight));          // 真尺寸口径不变
    QCOMPARE(worker->width(), kWidth);              // API 也仍报真尺寸
    QCOMPARE(worker->height(), kHeight);

    worker->stop();
}

// 运行途中改预算：之后的帧要跟着变小（窗口 resize 就走这条路径）。
void TestDecoderWorker::outputSizeChangeMidStreamTakesEffect()
{
    StreamLibrary lib;
    if (!lib.build())
        QSKIP("Media Foundation H.264 编解码不可用（无 MFT / Windows N），跳过");

    auto *worker = new lanrdp::DecoderWorker(kWidth, kHeight, this);
    worker->start();
    QVERIFY2(worker->waitReady(), qPrintable(worker->lastError()));

    // 先全尺寸解一段，确认基线
    QSignalSpy fullSpy(worker, &lanrdp::DecoderWorker::frameDecoded);
    QVERIFY2(feedAll(worker, lib, fullSpy), "baseline decode failed");
    QCOMPARE(fullSpy.last().at(0).value<QImage>().size(), QSize(kWidth, kHeight));

    // 改预算到 1/4 大小，再喂一轮
    worker->setOutputSize(QSize(kWidth / 4, kHeight / 4));

    QSignalSpy smallSpy(worker, &lanrdp::DecoderWorker::frameDecoded);
    for (const QByteArray &bs : lib.frames)
        worker->decode(bs);
    const auto isQuarter = [&smallSpy]() {
        return smallSpy.count() > 0
               && smallSpy.last().at(0).value<QImage>().size() == QSize(kWidth / 4, kHeight / 4);
    };
    QVERIFY2(pumpUntil(isQuarter), "mid-stream setOutputSize did not take effect");

    QCOMPARE(worker->width(), kWidth);   // 真尺寸依然不动
    QCOMPARE(worker->height(), kHeight);

    worker->stop();
}

// 非法尺寸：open 失败要能被 waitReady() 观察到，且带可读原因，
// 而不是让调用方以为「已经就绪」。
void TestDecoderWorker::reportsOpenFailureForInvalidSize()
{
    auto *worker = new lanrdp::DecoderWorker(0, 0, this);
    worker->start();

    QVERIFY2(!worker->waitReady(3000), "open(0,0) must not report ready");
    QVERIFY2(!worker->lastError().isEmpty(), "open failure must carry a readable reason");
    QVERIFY(!worker->isReady());
    qInfo("invalid-size open rejected: %s", qPrintable(worker->lastError()));

    // 线程应已自然退出（run() 里 open 失败后直接 return）
    QVERIFY(worker->wait(3000));
    QVERIFY(!worker->isRunning());

    worker->stop();  // 幂等
    delete worker;   // 析构不得 abort
}

// 生命周期：stop() 必须真的 join 掉线程；重复 stop() 幂等；析构不崩。
void TestDecoderWorker::stopJoinsThreadCleanly()
{
    auto *worker = new lanrdp::DecoderWorker(kWidth, kHeight, this);
    worker->start();
    QVERIFY2(worker->waitReady(), qPrintable(worker->lastError()));
    QVERIFY(worker->isRunning());

    worker->stop();
    QVERIFY2(!worker->isRunning(), "stop() must join the decoding thread");

    worker->stop();  // 幂等
    QVERIFY(!worker->isRunning());

    delete worker;

    // 再验一遍「不显式 stop 就析构」—— 析构里必须自己 join，否则 QThread 会 abort
    auto *second = new lanrdp::DecoderWorker(kWidth, kHeight, this);
    second->start();
    QVERIFY2(second->waitReady(), qPrintable(second->lastError()));
    QVERIFY(second->isRunning());
    delete second;  // 不调 stop()：析构里必须兜底
}

// 入队顺序必须保持 —— 乱序喂 Annex-B 会破坏参考帧链（P 帧依赖前序帧）。
// 用「帧序号」不可见，改为验证：同一批帧按序喂进去，产帧数量与串行解码一致。
void TestDecoderWorker::queuedFramesArriveInOrder()
{
    StreamLibrary lib;
    if (!lib.build())
        QSKIP("Media Foundation H.264 编解码不可用（无 MFT / Windows N），跳过");

    // 基线：同线程串行解码，**逐包**记下产出帧数（下面按序核对要用）
    // ⚠️ `images` 必须每轮新建 —— `decodeFrame` 是**追加**语义，复用会累加出虚高基线
    QVector<int> perPacket;
    perPacket.reserve(lib.frames.size());
    int serialCount = 0;
    {
        lanrdp::H264Decoder decoder;
        QVERIFY2(decoder.open(kWidth, kHeight), qPrintable(decoder.lastError()));
        for (const QByteArray &bs : lib.frames) {
            QVector<QImage> images;
            QVERIFY2(decoder.decodeFrame(bs, images), qPrintable(decoder.lastError()));
            perPacket.append(images.size());
            serialCount += images.size();
        }
        decoder.close();
    }
    QVERIFY(serialCount > 0);
    qInfo("serial baseline produced %d frames from %d packets", serialCount, int(lib.frames.size()));

    // 工作线程：**逐包喂、逐包核对累计产出数**。若工作线程把包的处理次序弄乱，
    // 某个包的产出就会提前/滞后出现，累计数立刻对不上 —— 这才是「按序」的判据
    // （只比总数的话，乱序也能凑够总数，等于没验）。
    auto *worker = new lanrdp::DecoderWorker(kWidth, kHeight, this);
    worker->start();
    QVERIFY2(worker->waitReady(), qPrintable(worker->lastError()));

    QSignalSpy spy(worker, &lanrdp::DecoderWorker::frameDecoded);
    int expected = 0;
    for (int i = 0; i < lib.frames.size(); ++i) {
        worker->decode(lib.frames[i]);
        expected += perPacket[i];
        QVERIFY2(pumpUntil([&spy, expected]() { return spy.count() >= expected; }),
                 qPrintable(QStringLiteral("第 %1 包的产出未按时到达（期望累计 %2 帧）")
                                .arg(i).arg(expected)));
        QCOMPARE(spy.count(), expected);
    }
    QCOMPARE(spy.count(), serialCount);

    worker->stop();
}

// 启动超时只能「报告」，不能替线程把结论钉死。
// 线程随后真的起来了就必须能就绪 —— 否则明明在正常解码，上层却一直判它没就绪、
// 每帧都推倒重建一次。8 轮是为了让「超时」与「线程回填」尽量贴近、撞上那个窗口。
void TestDecoderWorker::readyTimeoutLeavesStateForRealResult()
{
    for (int i = 0; i < 8; ++i) {
        lanrdp::DecoderWorker worker(kWidth, kHeight);
        QVERIFY2(!worker.waitReady(0), "还没启动就该超时返回 false");
        QVERIFY2(!worker.isReady(), "超时之后不该报告就绪");

        worker.start();
        QVERIFY2(worker.waitReady(10000), qPrintable(worker.lastError()));
        QVERIFY2(worker.isReady(), "线程真正就绪后，先前那次超时不得留下「没就绪」的闩锁");
        worker.stop();
    }
}

QTEST_GUILESS_MAIN(TestDecoderWorker)
#include "test_decoderworker.moc"
