#include <QtTest/QtTest>

#include <QTcpServer>
#include <QTcpSocket>

#include "host/ScreenCapturer.h"
#include "host/ScreenStreamer.h"
#include "net/Session.h"
#include "protocol/Frame.h"
#include "protocol/ScreenFrame.h"

// 抓屏流分发到 Session
class TestStreamer : public QObject
{
    Q_OBJECT

private slots:
    void dispatchesEncodedFramesToTargets();
    void doesNotCaptureWithoutTargets();
    void backlogLowersBitrate();
    void noBacklogKeepsConfiguredBitrate();
    void backlogAtMinBitrateHalvesFps();

    // 抓屏失效后必须能自愈（这条用例此前完全缺失）
    void recoversAfterRecoverableCaptureLoss();
    void keepsRetryingWhileRecoverableLossPersists();
    void stopsAfterRepeatedFatalFailures();
    void noNewFrameIsNotAFailure();
    void initFailureIsRecoverableOnlyWhenMarkedSo();

    // 发送侧背压：慢目标被跳过、不影响快目标、持续超标被断开、裸帧也降帧率
    void congestedTargetIsSkipped();
    void congestionDoesNotAffectHealthyTarget();
    void persistentlyCongestedTargetIsDropped();
    void congestionDetectedWithoutNewFrames();
    void rawPathAlsoThrottlesFps();

    // H.264 编码失败后必须显式降级并停止重试（帧类型不得逐帧抖动）
    void h264FailureSwitchesToRawAndStopsRetrying();
    void codecFallbackIsEmittedExactlyOnce();
    void reselectingH264AfterFallbackRetries();
};

namespace {

QImage makeImage(int width, int height)
{
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixel(x, y, qRgba(x % 256, y % 256, (x + y) % 256, 255));
        }
    }
    return image;
}

// 建立 loopback 连接，返回 server 侧的 socket（被控端推送目标）。client 保持连接。
QTcpSocket *makeLoopbackServerSide(QTcpServer *server, QTcpSocket *client)
{
    if (!server->listen(QHostAddress::LocalHost, 0))
        return nullptr;
    client->connectToHost(QHostAddress::LocalHost, server->serverPort());
    if (!client->waitForConnected(3000))
        return nullptr;
    if (!server->waitForNewConnection(3000))
        return nullptr;
    return server->nextPendingConnection();
}

} // namespace

void TestStreamer::dispatchesEncodedFramesToTargets()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY(client.waitForConnected(3000));
    QVERIFY(server.waitForNewConnection(3000));

    QTcpSocket *serverSide = server.nextPendingConnection();
    QVERIFY(serverSide != nullptr);

    lanrdp::Session session(serverSide);  // 被控端侧的推送目标

    QByteArray received;
    QObject::connect(&client, &QTcpSocket::readyRead,
                     [&received, &client]() { received.append(client.readAll()); });

    const QImage expected = makeImage(64, 48);

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([&expected](QImage *out) {
        *out = expected;
        return lanrdp::CaptureStatus::Ok;
    });
    streamer.setIntervalMs(10);
    streamer.addTarget(&session);
    QCOMPARE(streamer.targetCount(), 1);

    streamer.start();
    QVERIFY(streamer.isRunning());

    const int expectedSize = lanrdp::kRawFrameHeaderSize + 64 * 48 * 4;
    QTRY_VERIFY_WITH_TIMEOUT(received.size() >= expectedSize, 5000);

    streamer.stop();
    QVERIFY(!streamer.isRunning());

    lanrdp::Frame frame;
    qsizetype     consumed = 0;
    QCOMPARE(lanrdp::Frame::tryParse(received, &frame, &consumed), lanrdp::ParseStatus::Ok);
    QCOMPARE(frame.type, static_cast<quint8>(lanrdp::FrameType::ScreenFrameRaw));

    const QImage decoded = lanrdp::decodeRawFrame(frame.payload);
    QVERIFY(!decoded.isNull());
    QCOMPARE(decoded.size(), expected.size());
    QCOMPARE(decoded.pixel(7, 9), expected.pixel(7, 9));

    QVERIFY(streamer.framesSent() >= 1);
    QVERIFY(streamer.bytesSent() >= static_cast<quint64>(frame.payload.size()));
}

void TestStreamer::doesNotCaptureWithoutTargets()
{
    int calls = 0;

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([&calls](QImage *out) {
        ++calls;
        *out = makeImage(4, 4);
        return lanrdp::CaptureStatus::Ok;
    });
    streamer.setIntervalMs(10);

    streamer.start();
    QTest::qWait(120);
    streamer.stop();

    QCOMPARE(calls, 0);  // 没有观看者就不该抓屏
}

// 积压超水位时，自适应码率应下降（保流畅优先，降码率）。
void TestStreamer::backlogLowersBitrate()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *out) { *out = makeImage(320, 240); return lanrdp::CaptureStatus::Ok; });
    streamer.setIntervalMs(30);
    streamer.setH264Params(30, 8 * 1000 * 1000, 2);
    streamer.setCodec(lanrdp::StreamCodec::H264);
    streamer.setPendingSource([]() { return qint64(3 * 1024 * 1024); });  // 超水位 2MB
    streamer.addTarget(&session);

    streamer.start();
    QTRY_VERIFY_WITH_TIMEOUT(streamer.h264Active(), 5000);   // 编码器已创建
    QTRY_VERIFY_WITH_TIMEOUT(streamer.adaptiveBitrate() < 8 * 1000 * 1000, 5000);
    streamer.stop();
}

// 无积压时不降码率，保持用户设的值。
void TestStreamer::noBacklogKeepsConfiguredBitrate()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *out) { *out = makeImage(320, 240); return lanrdp::CaptureStatus::Ok; });
    streamer.setIntervalMs(30);
    streamer.setH264Params(30, 8 * 1000 * 1000, 2);
    streamer.setCodec(lanrdp::StreamCodec::H264);
    streamer.setPendingSource([]() { return qint64(0); });  // 无积压
    streamer.addTarget(&session);

    streamer.start();
    QTRY_VERIFY_WITH_TIMEOUT(streamer.h264Active(), 5000);
    QTest::qWait(200);  // 给几个 tick 的机会，确认不降
    QCOMPARE(streamer.adaptiveBitrate(), 8 * 1000 * 1000);
    streamer.stop();
}

// 码率已到下限仍积压时，降帧率（抓帧间隔翻倍）。
void TestStreamer::backlogAtMinBitrateHalvesFps()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *out) { *out = makeImage(320, 240); return lanrdp::CaptureStatus::Ok; });
    streamer.setIntervalMs(50);                            // 基准 20fps
    streamer.setH264Params(20, 1 * 1000 * 1000, 2);        // 1M：一个 tick 降到 500k 下限
    streamer.setCodec(lanrdp::StreamCodec::H264);
    streamer.setPendingSource([]() { return qint64(3 * 1024 * 1024); });
    streamer.addTarget(&session);

    streamer.start();
    QTRY_VERIFY_WITH_TIMEOUT(streamer.h264Active(), 5000);
    // 码率 1M→500k（下限），再降帧率 20→10，间隔 50ms→100ms
    QTRY_COMPARE_WITH_TIMEOUT(streamer.intervalMs(), 100, 5000);
    streamer.stop();
}

// ---------------------------------------------------------------------------
// 抓屏失效后的自愈
//
// 病灶：`onTick` 抓到第一个错误就 `stop()`，且 `captureFrame` 只在
// `capturer_ == nullptr` 时初始化 ⇒ 死掉的 DXGI 复制对象**永不重建**。而 ACCESS_LOST
// 恰恰是 UAC 安全桌面 / 锁屏解锁 / 分辨率变化 / 显示器唤醒时的**日常**返回值 ——
// 净结果就是「弹一次 UAC，画面永久冻结，只能重启程序」。
//
// 下面四条就是这条缺陷的守卫。
// ---------------------------------------------------------------------------

// ① 可恢复失效（ACCESS_LOST 一类）→ 必须自行恢复并重新推帧，不得停流。
void TestStreamer::recoversAfterRecoverableCaptureLoss()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    int calls = 0;
    int interrupted = 0, resumed = 0, failedCount = 0;

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([&calls](QImage *out) {
        ++calls;
        if (calls <= 2)  // 前两次模拟「UAC 弹了一下」
            return lanrdp::CaptureStatus::RecoverableLost;
        *out = makeImage(32, 24);
        return lanrdp::CaptureStatus::Ok;
    });
    streamer.setIntervalMs(10);
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::captureInterrupted,
                     [&interrupted](const QString &) { ++interrupted; });
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::captureResumed,
                     [&resumed]() { ++resumed; });
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::failed,
                     [&failedCount](const QString &) { ++failedCount; });
    streamer.addTarget(&session);

    streamer.start();
    QTRY_VERIFY_WITH_TIMEOUT(streamer.framesSent() >= 1, 5000);  // 失效之后又推上帧了

    QVERIFY2(streamer.isRunning(),
             "抓屏短暂失效后必须继续运行 —— 旧实现在这里 stop()，画面就永久冻死了");
    QVERIFY(interrupted >= 1);
    QVERIFY(resumed >= 1);
    QCOMPARE(failedCount, 0);  // 可恢复的失效不该被当成「放弃」
    streamer.stop();
}

// ② 可恢复失效**持续存在**时也不放弃 —— 锁屏可能长达数分钟。
void TestStreamer::keepsRetryingWhileRecoverableLossPersists()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    int interrupted = 0, failedCount = 0;

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *) { return lanrdp::CaptureStatus::RecoverableLost; });
    streamer.setIntervalMs(10);
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::captureInterrupted,
                     [&interrupted](const QString &) { ++interrupted; });
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::failed,
                     [&failedCount](const QString &) { ++failedCount; });
    streamer.addTarget(&session);

    streamer.start();
    QTest::qWait(500);  // 期间会失败很多次

    QVERIFY2(streamer.isRunning(),
             "可恢复失效持续存在时也不能停流 —— 锁屏能持续数分钟，停了就再也回不来");
    QCOMPARE(failedCount, 0);
    QCOMPARE(interrupted, 1);  // 只在「中断」跳变点发一次，不刷屏
    streamer.stop();
}

// ③ 非可恢复失败（配置 / 资源类）连续 N 次仍无起色才停 —— 保留既有 failed 语义。
void TestStreamer::stopsAfterRepeatedFatalFailures()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    int interrupted = 0, failedCount = 0;

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *) { return lanrdp::CaptureStatus::Failed; });
    streamer.setIntervalMs(10);
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::captureInterrupted,
                     [&interrupted](const QString &) { ++interrupted; });
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::failed,
                     [&failedCount](const QString &) { ++failedCount; });
    streamer.addTarget(&session);

    streamer.start();
    QTRY_VERIFY_WITH_TIMEOUT(!streamer.isRunning(), 5000);  // 最终确实放弃了

    QCOMPARE(failedCount, 1);
    QVERIFY(interrupted >= 1);      // 放弃之前应当先报过「中断」
    QCOMPARE(streamer.framesSent(), quint64(0));
    streamer.stop();                // 幂等
}

// ④ 「桌面无变化」不是失败 —— 回归护栏（不能把超时也当成失效）。
void TestStreamer::noNewFrameIsNotAFailure()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    int interrupted = 0, failedCount = 0;

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *) { return lanrdp::CaptureStatus::NoNewFrame; });
    streamer.setIntervalMs(10);
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::captureInterrupted,
                     [&interrupted](const QString &) { ++interrupted; });
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::failed,
                     [&failedCount](const QString &) { ++failedCount; });
    streamer.addTarget(&session);

    streamer.start();
    QTest::qWait(200);

    QVERIFY(streamer.isRunning());
    QCOMPARE(interrupted, 0);
    QCOMPARE(failedCount, 0);
    streamer.stop();
}

// 「创建即失败」也得分档：可恢复的（启动时锁屏 / 被远控软件独占显示输出）必须映射成
// RecoverableLost，才能走「限频重试、不停流」那条路；否则连续几次就被判死、要手动重启监听。
void TestStreamer::initFailureIsRecoverableOnlyWhenMarkedSo()
{
    QCOMPARE(lanrdp::ScreenStreamer::captureStatusForInitFailure(true),
             lanrdp::CaptureStatus::RecoverableLost);
    QCOMPARE(lanrdp::ScreenStreamer::captureStatusForInitFailure(false),
             lanrdp::CaptureStatus::Failed);
}

// ---------------------------------------------------------------------------
// 发送侧背压
//
// 病灶：`onTick` 向每个 target **无条件**推帧，从不看 `pendingBytes()`。
// 4K 裸帧一帧 ≈ 33 MB，按 10 fps 推给一个接收慢的对端 ⇒ 每秒数百 MB 堆在
// socket 写缓冲里，几秒就把被控端进程拖到 OOM。
// 下面四条守卫「跳过慢目标 / 不影响快目标 / 持续超标断开 / 裸帧也降帧率」。
// ---------------------------------------------------------------------------

// 积压超上限的目标：本帧跳过（它收到的字节数不因推帧而增长）。
void TestStreamer::congestedTargetIsSkipped()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *out) { *out = makeImage(640, 480); return lanrdp::CaptureStatus::Ok; });
    streamer.setIntervalMs(10);
    // 积压远超 8 MB 上限。
    streamer.setPendingSource([]() { return qint64(64 * 1024 * 1024); });
    streamer.addTarget(&session);

    streamer.start();
    QTest::qWait(200);
    streamer.stop();

    QCOMPARE(streamer.framesSent(), quint64(0));  // 一帧都没推出去
}

// 一个目标拥塞不得影响另一个健康目标（背压是**每目标**判，不是全局）。
void TestStreamer::congestionDoesNotAffectHealthyTarget()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    QByteArray received;
    QObject::connect(&client, &QTcpSocket::readyRead,
                     [&received, &client]() { received.append(client.readAll()); });

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *out) { *out = makeImage(64, 48); return lanrdp::CaptureStatus::Ok; });
    streamer.setIntervalMs(10);
    streamer.setPendingSource([]() { return qint64(0); });  // 健康：无积压
    streamer.addTarget(&session);

    streamer.start();
    const int expectedSize = lanrdp::kRawFrameHeaderSize + 64 * 48 * 4;
    QTRY_VERIFY_WITH_TIMEOUT(received.size() >= expectedSize, 5000);
    streamer.stop();

    QVERIFY(streamer.framesSent() >= 1);  // 健康目标照收
}

// 持续超标 ⇒ 断开该目标，且发出 failed（只断超标那条）。
void TestStreamer::persistentlyCongestedTargetIsDropped()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    int failedCount = 0;
    QString failReason;

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *out) { *out = makeImage(64, 48); return lanrdp::CaptureStatus::Ok; });
    streamer.setIntervalMs(20);
    streamer.setPendingSource([]() { return qint64(64 * 1024 * 1024); });
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::targetCongested,
                     [&failedCount, &failReason](lanrdp::Session *, const QString &why) {
                         ++failedCount;
                         failReason = why;
                     });
    streamer.addTarget(&session);

    streamer.start();
    // 超标后按**墙钟**计时，满 5 秒即放弃 —— 与抓帧间隔、出帧率都无关。
    QTRY_VERIFY_WITH_TIMEOUT(failedCount >= 1, 8000);
    streamer.stop();

    QVERIFY(!failReason.isEmpty());
}

// 桌面静止（连续 NoNewFrame）时也必须能识别持续积压。
// 判定若只挂在「有帧要发」的路径上，卡死的接收端会一直占着会话槽不被断开。
void TestStreamer::congestionDetectedWithoutNewFrames()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    int failedCount = 0;

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *) { return lanrdp::CaptureStatus::NoNewFrame; });
    streamer.setIntervalMs(20);
    streamer.setPendingSource([]() { return qint64(64 * 1024 * 1024); });
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::targetCongested,
                     [&failedCount](lanrdp::Session *, const QString &) { ++failedCount; });
    streamer.addTarget(&session);

    streamer.start();
    QTRY_VERIFY_WITH_TIMEOUT(failedCount >= 1, 8000);
    streamer.stop();

    QVERIFY(failedCount >= 1);
    QCOMPARE(streamer.framesSent(), quint64(0));  // 一帧都没产出，判定照样成立
}

// 裸帧路径也必须参与背压（降帧率，而不是像 H.264 那样降码率）。
void TestStreamer::rawPathAlsoThrottlesFps()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *out) { *out = makeImage(320, 240); return lanrdp::CaptureStatus::Ok; });
    streamer.setIntervalMs(50);                       // 基准 20fps
    streamer.setCodec(lanrdp::StreamCodec::RawFrames); // 裸帧
    streamer.setPendingSource([]() { return qint64(3 * 1024 * 1024); });  // 超水位 2MB
    streamer.addTarget(&session);

    streamer.start();
    // 裸帧无码率可降 ⇒ 应直接降帧率：间隔 50ms → 100ms
    QTRY_COMPARE_WITH_TIMEOUT(streamer.intervalMs(), 100, 5000);
    streamer.stop();
}

// ---------------------------------------------------------------------------
// H.264 编码失败后的降级
//
// 病灶：`encodeH264` 失败时 `onTick` 不 return，同一 tick 直落裸帧路径；下一 tick
// 又回到 H.264 再试 ⇒ **帧类型逐帧抖动**，而 `codec()` 一直宣称 H.264（对外撒谎）。
// 修法：首次失败即**显式** `codec_ = RawFrames` + 发一次 `codecFallback`，此后不再试。
// 用**奇数尺寸**做确定性触发：`encodeH264` 在创建编码器之前就因 NV12 不支持而失败，
// 无需真实 Media Foundation 环境。
// ---------------------------------------------------------------------------

// 首次失败 ⇒ 切到裸帧，且不再产生 H.264 帧（帧类型稳定）。
void TestStreamer::h264FailureSwitchesToRawAndStopsRetrying()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    QByteArray received;
    QObject::connect(&client, &QTcpSocket::readyRead,
                     [&received, &client]() { received.append(client.readAll()); });

    lanrdp::ScreenStreamer streamer;
    // 奇数尺寸 ⇒ H.264 编码必然失败（NV12 不支持）。
    streamer.setSource([](QImage *out) { *out = makeImage(63, 49); return lanrdp::CaptureStatus::Ok; });
    streamer.setIntervalMs(20);
    streamer.setCodec(lanrdp::StreamCodec::H264);
    streamer.addTarget(&session);

    streamer.start();
    // 一帧裸帧（63*49*4 + 头）就足够证明「已在发裸帧」。
    const int rawFrameSize = lanrdp::kRawFrameHeaderSize + 63 * 49 * 4;
    QTRY_VERIFY_WITH_TIMEOUT(received.size() >= rawFrameSize, 5000);
    streamer.stop();

    QCOMPARE(streamer.effectiveCodec(), lanrdp::StreamCodec::RawFrames);
    QVERIFY2(!streamer.h264Active(), "降级后不该还宣称 H.264 生效");

    // 收到的帧**全是**裸帧，没有 H.264 帧混进来。
    qsizetype     consumed = 0;
    int           raw      = 0;
    int           h264     = 0;
    lanrdp::Frame frame;
    while (lanrdp::Frame::tryParse(received, &frame, &consumed) == lanrdp::ParseStatus::Ok) {
        if (frame.type == static_cast<quint8>(lanrdp::FrameType::ScreenFrameRaw))
            ++raw;
        else if (frame.type == static_cast<quint8>(lanrdp::FrameType::ScreenFrameH264))
            ++h264;
        received.remove(0, consumed);
        consumed = 0;
    }
    QVERIFY2(raw >= 1, "应当发裸帧");
    QCOMPARE(h264, 0);
}

// codecFallback 只发一次（多 tick 后仍是 1）—— 不能每帧都报。
void TestStreamer::codecFallbackIsEmittedExactlyOnce()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    int fallbackCount = 0;

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([](QImage *out) { *out = makeImage(63, 49); return lanrdp::CaptureStatus::Ok; });
    streamer.setIntervalMs(10);
    streamer.setCodec(lanrdp::StreamCodec::H264);
    QObject::connect(&streamer, &lanrdp::ScreenStreamer::codecFallback,
                     [&fallbackCount](const QString &) { ++fallbackCount; });
    streamer.addTarget(&session);

    streamer.start();
    QTRY_VERIFY_WITH_TIMEOUT(fallbackCount >= 1, 5000);
    QTest::qWait(200);  // 再给几十个 tick 的机会，确认不会重复报
    streamer.stop();

    QCOMPARE(fallbackCount, 1);
}

// 降级后用户再次选 H.264 ⇒ 必须允许重新尝试（不能永久锁死在裸帧）。
void TestStreamer::reselectingH264AfterFallbackRetries()
{
    QTcpServer server;
    QTcpSocket client;
    QTcpSocket *serverSide = makeLoopbackServerSide(&server, &client);
    QVERIFY(serverSide != nullptr);
    lanrdp::Session session(serverSide);

    bool oddSize = true;

    lanrdp::ScreenStreamer streamer;
    streamer.setSource([&oddSize](QImage *out) {
        *out = oddSize ? makeImage(63, 49) : makeImage(64, 48);
        return lanrdp::CaptureStatus::Ok;
    });
    streamer.setIntervalMs(20);
    streamer.setCodec(lanrdp::StreamCodec::H264);
    streamer.addTarget(&session);

    streamer.start();
    QTRY_COMPARE_WITH_TIMEOUT(streamer.effectiveCodec(), lanrdp::StreamCodec::RawFrames, 5000);

    // 修好尺寸 + 重新勾选 H.264 ⇒ 应当可以重新尝试（h264Active 变真）。
    oddSize = false;
    streamer.setCodec(lanrdp::StreamCodec::RawFrames);   // 先显式回裸帧（模拟用户取消勾选）
    streamer.setCodec(lanrdp::StreamCodec::H264);        // 再勾上
    QTRY_VERIFY_WITH_TIMEOUT(streamer.h264Active(), 5000);
    streamer.stop();
}

QTEST_GUILESS_MAIN(TestStreamer)
#include "test_streamer.moc"
