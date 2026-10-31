#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QSignalSpy>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "host/AudioCapturer.h"
#include "host/HostController.h"
#include "net/ClientConnection.h"
#include "protocol/AudioChunk.h"
#include "viewer/AudioPlayer.h"

#include <cmath>

// 音频的**硬件往返**：loopback 采集要真抓得到声音、渲染端要真打得开设备。
//
// 这一层故意不做打桩 —— WASAPI 是唯一「单测不了、又最容易写错」的部分
//（静默期不出事件、设备失效、事件驱动时序），打桩等于把要测的东西全盖掉。
// 没有可用音频设备的机器上**跳过**（QSKIP），不是失败。
class TestAudioHw : public QObject
{
    Q_OBJECT

private slots:
    // 播放端「一拍写多少帧」是**纯函数**：不碰 COM，没有声卡也照跑（不会 QSKIP）。
    // 延迟是否失控全由这几条守住。
    void framesToWriteTargetsQueueDepth();
    void framesToWriteNeverExceedsFreeSpace();
    void framesToWriteNeverExceedsAvailableData();
    void framesToWriteAlignsWholeFrames();
    void framesToWriteWritesNothingWhenTargetMet();
    void framesToWriteRejectsBadInput();

    void capturerOpensAndReportsFormat();
    void capturesToneFromLoopback();
    void capturerStopsQuickly();
    void playerOpensAndStops();
    // 闭环：播放端写出去的声音，能被本机 loopback 采回来。
    // 这是**唯一**不靠耳朵也能证明「真的出声了」的判据。
    void playerOutputReachesLoopback();
    // 延迟回归：设备队列必须常驻在浅深度，不能顶在 client 缓冲长度上。
    void playerKeepsDeviceQueueShallow();
    // 切格掐尾巴：`reset()` 得真清设备里已排的数据，只清内存不算。
    void resetFlushesDeviceQueue();
    // 换格式不许卡：旧线程停不下来就该放弃，而不是白等 3 秒再「假装成功」。
    void beginSwitchesFormatWithoutStalling();
    // 端到端：被控端采集 → 线上 → 控制端解出音频帧
    void audioFlowsFromHostToClient();
};

namespace {

lanrdp::AudioFormat stereo48k()
{
    lanrdp::AudioFormat f;
    f.sampleRate    = 48000;
    f.channels      = 2;
    f.bitsPerSample = 16;
    f.blockMs       = 20;
    return f;
}

// 起采集并等到格式就绪。返回 false = 本机没有可用的音频输出设备（调用方 QSKIP）。
bool startCapturer(lanrdp::AudioCapturer *capturer)
{
    QSignalSpy formats(capturer, &lanrdp::AudioCapturer::formatChanged);
    capturer->startCapture();   // 唯一启动入口（停机标志在这条路径上清零）
    if (!formats.wait(3000))
    {
        capturer->stopAndWait();
        return false;
    }
    return true;
}

// 440 Hz 正弦，S16LE 立体声交织，长度 ms 毫秒。
QByteArray makeTone(int ms, int sampleRate, int channels)
{
    QByteArray out;
    const int  frames = sampleRate * ms / 1000;
    for (int i = 0; i < frames; ++i)
    {
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 * i / sampleRate;
        const auto   s     = static_cast<qint16>(std::sin(phase) * 0.5 * 32767.0);
        for (int c = 0; c < channels; ++c)
        {
            out.append(static_cast<char>(s & 0xFF));
            out.append(static_cast<char>((s >> 8) & 0xFF));
        }
    }
    return out;
}

bool hasNonZeroSample(const QByteArray &pcm)
{
    for (char c : pcm)
    {
        if (c != 0)
            return true;
    }
    return false;
}

} // namespace

// —— 播放节流（纯逻辑）————————————————————————————————————————————
//
// 这三个数是 48 kHz 立体声 S16 下的实测口径：client 缓冲 100 ms = 4800 帧、
// 目标深度 50 ms = 2400 帧、每帧 4 字节。

void TestAudioHw::framesToWriteTargetsQueueDepth()
{
    // 队列空、数据管够 ⇒ 只写到目标深度，**不写满 4800 帧的空闲区**。
    QCOMPARE(lanrdp::framesToWrite(0, 4800, 2400, 11520, 4), 2400);
    // 已有 20 ms ⇒ 只再补 30 ms。
    QCOMPARE(lanrdp::framesToWrite(960, 4800, 2400, 11520, 4), 1440);
}

void TestAudioHw::framesToWriteNeverExceedsFreeSpace()
{
    // 目标是深于 client 缓冲的误配置（换了设备、缓冲被压小）：仍不能越界写。
    QCOMPARE(lanrdp::framesToWrite(900, 1000, 2400, 11520, 4), 100);
    // 缓冲满 ⇒ 一帧都不写。
    QCOMPARE(lanrdp::framesToWrite(1000, 1000, 2400, 11520, 4), 0);
}

void TestAudioHw::framesToWriteNeverExceedsAvailableData()
{
    // 抖动缓冲里只有 20 ms ⇒ 只能写 960 帧。**不足部分补静音是错的**：
    // 一补，队列就顶到目标深度却全是静音，声音晚到 50 ms 才出声。
    QCOMPARE(lanrdp::framesToWrite(0, 4800, 2400, 960 * 4, 4), 960);
    // 一个字节都没有 ⇒ 不写（不是「写一整段静音」）。
    QCOMPARE(lanrdp::framesToWrite(0, 4800, 2400, 0, 4), 0);
}

void TestAudioHw::framesToWriteAlignsWholeFrames()
{
    // 尾部半帧不能进设备：按整帧向下取整（向上取整会把下一个块的首字节切进来）。
    QCOMPARE(lanrdp::framesToWrite(0, 4800, 2400, 960 * 4 - 1, 4), 959);
    QCOMPARE(lanrdp::framesToWrite(0, 4800, 2400, 960 * 4 - 4, 4), 959);
}

void TestAudioHw::framesToWriteWritesNothingWhenTargetMet()
{
    QCOMPARE(lanrdp::framesToWrite(2400, 4800, 2400, 11520, 4), 0);
    QCOMPARE(lanrdp::framesToWrite(3000, 4800, 2400, 11520, 4), 0);
}

void TestAudioHw::framesToWriteRejectsBadInput()
{
    // 未配置 / 非法格式：一律不写，绝不返回负数。
    QCOMPARE(lanrdp::framesToWrite(0, 4800, 2400, 11520, 0), 0);
    QCOMPARE(lanrdp::framesToWrite(0, 0, 2400, 11520, 4), 0);
    QCOMPARE(lanrdp::framesToWrite(0, 4800, 0, 11520, 4), 0);
    QCOMPARE(lanrdp::framesToWrite(5000, 4800, 2400, 11520, 4), 0);
}

void TestAudioHw::capturerOpensAndReportsFormat()
{
    lanrdp::AudioCapturer capturer;
    if (!startCapturer(&capturer))
        QSKIP("本机没有可用的音频输出设备（无法做 loopback 采集）");

    const lanrdp::AudioFormat format = capturer.format();
    QVERIFY(lanrdp::isValidAudioFormat(format));
    QVERIFY(lanrdp::audioBlockBytes(format) > 0);
    QVERIFY(capturer.lastError().isEmpty());

    capturer.stopAndWait();
}

void TestAudioHw::capturesToneFromLoopback()
{
    lanrdp::AudioCapturer capturer;
    if (!startCapturer(&capturer))
        QSKIP("本机没有可用的音频输出设备（无法做 loopback 采集）");

    QSignalSpy chunks(&capturer, &lanrdp::AudioCapturer::chunkReady);

    // `Beep` 在 Vista 之后走**默认渲染设备**，因此能被 loopback 抓到 ——
    // 这是「不用外放、不依赖声卡混音设置」的最省事音源。
    ::Beep(440, 600);

    // 允许几拍余量：采集侧还有 20 ms 的攒块与设备周期。
    QVERIFY2(chunks.wait(3000), "loopback 采不到任何音频块（采集链路断了）");

    const QList<QVariant> args = chunks.takeFirst();
    QCOMPARE(args.size(), 3);
    const auto pcm = args.at(1).toByteArray();
    QVERIFY(!pcm.isEmpty());

    // 块长必须等于格式算出来的长度 —— 攒块器与格式换算对不上时，
    // 播放端会按错的对齐解析，听起来是噪声。
    QCOMPARE(pcm.size(), lanrdp::audioBlockBytes(capturer.format()));

    capturer.stopAndWait();
}

void TestAudioHw::capturerStopsQuickly()
{
    lanrdp::AudioCapturer capturer;
    if (!startCapturer(&capturer))
        QSKIP("本机没有可用的音频输出设备（无法做 loopback 采集）");

    QElapsedTimer timer;
    timer.start();
    capturer.stopAndWait();
    // 采集循环最长阻塞 50 ms；给足余量仍应在 1 s 内收口。
    // 这条守的是「退出程序不挂」——音频线程不能把停机拖住。
    QVERIFY2(timer.elapsed() < 1000,
             qPrintable(QStringLiteral("音频采集线程停了 %1 ms（应 < 1000）").arg(timer.elapsed())));
    QVERIFY(!capturer.isRunning());
}

void TestAudioHw::playerOpensAndStops()
{
    lanrdp::AudioPlayer player;
    QSignalSpy         failures(&player, &lanrdp::AudioPlayer::failed);

    QVERIFY(player.begin(stereo48k()));

    // 喂两秒静音：目的是「让渲染线程真的转起来」，不是听声音。
    const QByteArray block(lanrdp::audioBlockBytes(stereo48k()), '\0');
    for (int i = 0; i < 100; ++i)
        player.push(block);

    QTest::qWait(300);

    if (!failures.isEmpty())
        QSKIP(qPrintable(QStringLiteral("本机无法打开音频输出：%1")
                             .arg(failures.at(0).at(0).toString())));

    QVERIFY(player.isActive());

    QElapsedTimer timer;
    timer.start();
    player.stopAndWait();
    QVERIFY2(timer.elapsed() < 1000,
             qPrintable(QStringLiteral("音频播放线程停了 %1 ms（应 < 1000）").arg(timer.elapsed())));
    QVERIFY(!player.isRunning());
}

void TestAudioHw::playerOutputReachesLoopback()
{
    // 先起采集（loopback 抓默认渲染端），再让播放端放一段 440 Hz。
    lanrdp::AudioCapturer capturer;
    if (!startCapturer(&capturer))
        QSKIP("本机没有可用的音频输出设备（无法做 loopback 采集）");
    QSignalSpy chunks(&capturer, &lanrdp::AudioCapturer::chunkReady);

    lanrdp::AudioPlayer player;
    QSignalSpy         failures(&player, &lanrdp::AudioPlayer::failed);
    QVERIFY(player.begin(stereo48k()));

    const qint64 blockBytes = lanrdp::audioBlockBytes(stereo48k());
    const QByteArray tone   = makeTone(500, 48000, 2);
    for (int offset = 0; offset < tone.size(); offset += blockBytes)
        player.push(tone.mid(offset, blockBytes));

    QTest::qWait(100);
    if (!failures.isEmpty())
        QSKIP(qPrintable(QStringLiteral("本机打不开音频输出：%1")
                             .arg(failures.at(0).at(0).toString())));

    // 判据 = 采回来的数据里出现非零样本。这是「设备真的把这段声音合成出去了」的
    // 直接证据 —— 播放端自己的缓冲里有没有数据并不能说明它有没有到达设备。
    bool          sawSignal = false;
    QElapsedTimer timer;
    timer.start();
    while (!sawSignal && timer.elapsed() < 3000)
    {
        if (!chunks.wait(200))
            continue;
        while (!chunks.isEmpty())
        {
            if (hasNonZeroSample(chunks.takeFirst().at(1).toByteArray()))
            {
                sawSignal = true;
                break;
            }
        }
    }

    player.stopAndWait();
    capturer.stopAndWait();

    QVERIFY2(sawSignal,
             "播放端没报错，但 loopback 采回来的全是静音 —— 声音没到设备"
             "（常见原因：默认输出设备被静音 / 设成了「无输出」）");
}

void TestAudioHw::playerKeepsDeviceQueueShallow()
{
    lanrdp::AudioPlayer player;
    QSignalSpy         failures(&player, &lanrdp::AudioPlayer::failed);
    QVERIFY(player.begin(stereo48k()));

    // 按真实速度一拍拍喂 20 ms：稳态队列深度才有意义（一次灌 1 s 会立刻被
    // 抖动缓冲的上限裁掉，量到的是裁剪结果而不是节流结果）。
    const QByteArray block = makeTone(20, 48000, 2);

    // 先等设备真的打开。**把「慢」和「坏」分开**：设备起不来是 QSKIP，
    // 不是「队列为 0」这种看不懂的失败（音频服务刚被别的用例折腾过时会慢几秒）。
    QElapsedTimer warmup;
    warmup.start();
    while (player.queuedMs() == 0 && warmup.elapsed() < 5000)
    {
        player.push(block);
        QTest::qWait(20);
    }
    if (!failures.isEmpty())
        QSKIP(qPrintable(QStringLiteral("本机打不开音频输出：%1")
                             .arg(failures.at(0).at(0).toString())));
    QVERIFY2(player.queuedMs() > 0, "播放线程 5 秒内没往设备写出任何数据");

    int peak = 0;
    for (int tick = 0; tick < 25; ++tick)      // 25 × 20 ms = 500 ms 稳态
    {
        player.push(block);
        QTest::qWait(20);
        peak = qMax(peak, player.queuedMs());
    }

    player.stopAndWait();

    // 队列必须浅：目标 50 ms，留 70 ms 的调度余量。
    // 判定线 120 ms 之下是 client 缓冲 100 ms —— 一旦有人把「空闲区写满」
    // 改回来，队列立刻顶到 100 ms 附近并从这里漏出去（改前实测 180 ms）。
    QVERIFY2(peak <= 120,
             qPrintable(QStringLiteral("设备队列峰值 %1 ms（应 ≤ 120）—— "
                                       "延迟被钉在 client 缓冲上了")
                            .arg(peak)));
}

void TestAudioHw::resetFlushesDeviceQueue()
{
    lanrdp::AudioPlayer player;
    QSignalSpy         failures(&player, &lanrdp::AudioPlayer::failed);
    QVERIFY(player.begin(stereo48k()));

    const QByteArray block = makeTone(20, 48000, 2);
    for (int i = 0; i < 10; ++i)
        player.push(block);

    // 先确认队列里真有数据，否则「清空」是空断言。设备打不开则跳过。
    QElapsedTimer warmup;
    warmup.start();
    while (player.queuedMs() < 30 && failures.isEmpty() && warmup.elapsed() < 5000)
        QTest::qWait(2);
    if (!failures.isEmpty())
        QSKIP(qPrintable(QStringLiteral("本机打不开音频输出：%1")
                             .arg(failures.at(0).at(0).toString())));
    QVERIFY2(player.queuedMs() >= 30, "设备队列太浅，测不出「立刻清空」");

    const int     depthBefore   = player.queuedMs();
    const quint64 flushesBefore = player.flushCount();

    QElapsedTimer sinceReset;
    sinceReset.start();
    player.reset();

    // 判据是**时序**而不是「最终归零」：自然排空要 depthBefore 毫秒，
    // 只有 Reset 真生效才会在一次设备节拍（~10 ms）内清空。
    const int window = qMax(12, depthBefore / 2);
    while (player.queuedMs() != 0 && sinceReset.elapsed() < window)
        QTest::qWait(2);

    QVERIFY2(player.queuedMs() == 0,
             qPrintable(QStringLiteral("reset() 后 %1 ms 队列还剩 %2 ms（自然排空要 %3 ms）")
                            .arg(window)
                            .arg(player.queuedMs())
                            .arg(depthBefore)));
    // `flushCount` **只在 SUCCEEDED(Reset) 时自增** ⇒ 它涨了就证明 Reset 真成功过；
    // 只有 queuedMs 归零的话，Reset 一直失败 + 自然排空也能让用例全绿。
    QVERIFY2(player.flushCount() > flushesBefore,
             "Reset 没成功：flushCount 只在 SUCCEEDED(Reset) 时自增");

    player.stopAndWait();
}

void TestAudioHw::beginSwitchesFormatWithoutStalling()
{
    // 换格式会先停旧线程再起新的。停机标志若在 `run()` 入口清零，「刚 start 就 stop」
    // 的这次停机会被清掉 ⇒ `wait(3000)` 白等三秒，然后 `start()` 对已在跑的线程是
    // 空操作 —— 结果是「旧线程 + 新格式」，正是最该避免的状态。
    // 判据用**耗时**：正常应在几十毫秒内换完，白等就会跨过 3 秒。
    lanrdp::AudioPlayer player;
    QVERIFY(player.begin(stereo48k()));

    lanrdp::AudioFormat other = stereo48k();
    other.sampleRate          = 44100;   // 不同格式才会走「停了再起」

    QElapsedTimer timer;
    timer.start();
    QVERIFY(player.begin(other));
    const qint64 elapsed = timer.elapsed();

    player.stopAndWait();

    QVERIFY2(elapsed < 1500,
             qPrintable(QStringLiteral("换格式用了 %1 ms（应 < 1500）：停机标志很可能被 "
                                       "`run()` 入口清掉了")
                            .arg(elapsed)));
}

void TestAudioHw::audioFlowsFromHostToClient()
{
    lanrdp::HostController host;
    QVERIFY(host.start(0, QStringLiteral("s3cret")));

    lanrdp::ClientConnection client;
    client.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("s3cret"));
    QTRY_VERIFY_WITH_TIMEOUT(client.ready(), 5000);

    // 采集只在「有控制端」时启动；起不来就是本机没设备。
    QTRY_VERIFY_WITH_TIMEOUT(host.audioCaptureRunning(), 3000);
    if (!host.audioCaptureRunning())
        QSKIP("本机没有可用的音频输出设备（被控端采集起不来）");

    QSignalSpy dataSpy(&client, &lanrdp::ClientConnection::audioDataReceived);

    // 让被控端真的有声音可抓（Beep 走默认渲染设备，会被 loopback 抓到）。
    ::Beep(660, 600);

    QVERIFY2(dataSpy.wait(4000), "控制端没收到任何音频帧 —— 采集/扇出/解帧这条链断了");

    // 配置帧必须**先于**数据帧到达（否则播放端不知道采样率）
    QVERIFY2(client.hasAudioFormat(), "还没收到音频配置帧就收到了数据帧");
    QVERIFY(lanrdp::isValidAudioFormat(client.audioFormat()));

    const QList<QVariant> args = dataSpy.takeFirst();
    QCOMPARE(args.size(), 2);
    QVERIFY(!args.at(1).toByteArray().isEmpty());

    client.disconnectNow();
    host.stop();
}

QTEST_GUILESS_MAIN(TestAudioHw)
#include "test_audiohw.moc"
