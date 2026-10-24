#include <QtTest/QtTest>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QImage>
#include <QSignalSpy>
#include <QStringList>
#include <QTcpSocket>
#include <QVector>

#include <functional>

#include "host/ColorConvert.h"
#include "host/H264Encoder.h"
#include "net/ClientConnection.h"
#include "net/HostServer.h"
#include "net/Session.h"
#include "protocol/H264Frame.h"
#include "viewer/H264Decoder.h"

// 端到端回归：ClientConnection 把 H.264 收到 -> 交给解码线程 -> frameReady 到上层。
//
// 为什么这个测试必须存在：ClientConnection 是**唯一**使用 H264Decoder 的
// 地方，却没有任何测试覆盖（它只被 MainWindow 用，而 MainWindow 的 UI 测试不碰真连接）。
// 改造时踩过一个「一帧都收不到」的坑（把解码投递到 UI 线程的事件循环，被解码器的
// 线程绑定拒掉）—— 这个测试就是拦住那类回归的网。
class TestClientConnectionH264 : public QObject
{
    Q_OBJECT

private slots:
    void decodesH264FramesIntoFrameReady();
    void displayBudgetReachesDecoderThroughWorker();
    void resolutionChangeRebuildsDecoder();
    void sizeFlipIsThrottled();
    // 心跳不能「收到就回」—— 两端都回会形成无限乒乓
    void heartbeatDoesNotPingPong();
};

namespace {

constexpr int kWidth  = 320;
constexpr int kHeight = 240;

QByteArray makeTwoToneBgra(int width, int height)
{
    QByteArray data(width * height * 4, '\0');
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            char *p = data.data() + (y * width + x) * 4;
            const bool top = y < height / 2;
            p[0] = static_cast<char>(top ? 0 : 255);
            p[1] = 0;
            p[2] = static_cast<char>(top ? 255 : 0);
            p[3] = static_cast<char>(0xFF);
        }
    }
    return data;
}

// 用真实编码器产一批 H.264 包（关键帧自带 SPS/PPS）
QVector<QByteArray> encodeStream(int width, int height)
{
    QVector<QByteArray> packets;

    const QByteArray bgra = makeTwoToneBgra(width, height);
    const QByteArray nv12 = lanrdp::bgraToNv12(
        reinterpret_cast<const uchar *>(bgra.constData()), width * 4, width, height);
    if (nv12.isEmpty())
        return packets;

    lanrdp::H264Encoder encoder;
    if (!encoder.open(width, height, 30, 2 * 1000 * 1000, 1))
        return packets;

    for (int i = 0; i < 20; ++i) {
        QVector<lanrdp::H264EncodedFrame> produced;
        if (!encoder.encodeFrame(reinterpret_cast<const uchar *>(nv12.constData()),
                                 nv12.size(), i == 0, produced))
            return packets;
        for (const auto &f : produced) {
            lanrdp::H264Frame frame;
            frame.width     = static_cast<quint32>(width);
            frame.height    = static_cast<quint32>(height);
            frame.keyframe  = f.keyframe;
            frame.bitstream = f.bitstream;
            packets.append(lanrdp::encodeH264Frame(frame));
        }
    }
    return packets;
}

// 把 host.sessions() 里与客户端套接字配对的那个服务端会话找出来
lanrdp::Session *serverSessionFor(const lanrdp::HostServer &host, const QTcpSocket *clientSock)
{
    for (lanrdp::Session *s : host.sessions()) {
        if (s->socket() && clientSock && s->socket()->peerPort() == clientSock->localPort())
            return s;
    }
    return nullptr;
}

// 日志捕获：直接在内存里收 QString（不要经 qPrintable —— 那个 8-bit 转换会把
// 中文丢成空串，断言会误判）。
struct LogCapture
{
    QStringList lines;

    explicit LogCapture(lanrdp::ClientConnection *conn)
    {
        QObject::connect(conn, &lanrdp::ClientConnection::logMessage, conn,
                         [this](const QString &line) { lines << line; });
    }

    bool contains(const QString &needle) const
    {
        for (const QString &line : lines) {
            if (line.contains(needle))
                return true;
        }
        return false;
    }

    int count(const QString &needle) const
    {
        int n = 0;
        for (const QString &line : lines) {
            if (line.contains(needle))
                ++n;
        }
        return n;
    }
};

bool pumpUntil(const std::function<bool()> &done, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

// 端到端场景脚手架：监听 + 连上 + 握手完成，拿到两侧对象
struct E2E
{
    lanrdp::HostServer       host{QStringLiteral("pw")};
    lanrdp::ClientConnection client;
    bool                     ok = false;

    lanrdp::Session *serverSession() const
    {
        return serverSessionFor(host, client.session() ? client.session()->socket() : nullptr);
    }

    bool start(quint16 fixedPort = 0)
    {
        if (!host.listen(fixedPort))
            return false;
        client.connectTo(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("pw"));
        ok = pumpUntil([this]() { return client.ready(); }, 5000);
        return ok;
    }
};

} // namespace

// 核心：服务端发 H.264，客户端应经解码线程产出 frameReady（带正确的真尺寸）。
void TestClientConnectionH264::decodesH264FramesIntoFrameReady()
{
    const QVector<QByteArray> packets = encodeStream(kWidth, kHeight);
    if (packets.isEmpty())
        QSKIP("Media Foundation H.264 编码器不可用（无 MFT / Windows N），跳过");

    E2E e2e;
    QVERIFY2(e2e.start(), "handshake with local HostServer failed");

    QSignalSpy frameSpy(&e2e.client, &lanrdp::ClientConnection::frameReady);
    LogCapture log(&e2e.client);

    lanrdp::Session *srv = e2e.serverSession();
    QVERIFY2(srv != nullptr, "server-side session not found");

    for (const QByteArray &payload : packets)
        srv->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::ScreenFrameH264), payload);

    QVERIFY2(pumpUntil([&frameSpy]() { return frameSpy.count() > 0; }),
             "client produced no frame from H.264 stream");

    const QList<QVariant> args = frameSpy.last();
    QCOMPARE(args.size(), 3);
    const QImage image  = args.at(0).value<QImage>();
    const QSize  source = args.at(2).value<QSize>();

    QVERIFY2(!image.isNull(), "decoded image is null");
    QCOMPARE(source, QSize(kWidth, kHeight));
    QCOMPARE(image.size(), QSize(kWidth, kHeight));  // 未设预算 => 与码流等大
    QCOMPARE(e2e.client.framesReceived() > 0, true);

    // 日志里应出现「解码线程就绪」—— 确认走的是线程化路径而不是别的路
    qInfo("captured %d log lines", int(log.lines.size()));
    for (const QString &line : log.lines)
        qInfo().noquote() << "  |" << line;
    QVERIFY2(log.contains(QStringLiteral("画面解码就绪")),
             qPrintable(log.lines.join(QLatin1Char('\n'))));

    qInfo("e2e: received %d frames, last source size %dx%d",
          int(frameSpy.count()), source.width(), source.height());
}

// 显示预算要真的穿过 ClientConnection -> DecoderWorker -> H264Decoder：
// 输出被缩小，但 sourceSize 仍是码流真尺寸（状态栏分辨率靠它）。
void TestClientConnectionH264::displayBudgetReachesDecoderThroughWorker()
{
    const QVector<QByteArray> packets = encodeStream(kWidth, kHeight);
    if (packets.isEmpty())
        QSKIP("Media Foundation H.264 编码器不可用（无 MFT / Windows N），跳过");

    E2E e2e;
    // 预算必须在首帧之前设好（窗口显示的路径就是这样：showEvent/resizeEvent 下发，
    // 解码器还是惰性创建的时候只记值，建好再补发）
    // ⚠️ 用 1/16 面积（= 1/4 线性尺寸）：收益守卫的阈值是面积比 ≤ 1/9 才真缩，
    // 1/4 面积（kWidth/2）会被刻意退回全尺寸 —— 那一点是曲线上的最差点。
    e2e.client.setDisplayBudget(QSize(kWidth / 4, kHeight / 4));
    QVERIFY2(e2e.start(), "handshake with local HostServer failed");

    QSignalSpy frameSpy(&e2e.client, &lanrdp::ClientConnection::frameReady);

    lanrdp::Session *srv = e2e.serverSession();
    QVERIFY2(srv != nullptr, "server-side session not found");
    for (const QByteArray &payload : packets)
        srv->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::ScreenFrameH264), payload);

    QVERIFY2(pumpUntil([&frameSpy]() { return frameSpy.count() > 0; }),
             "client produced no frame from H.264 stream");

    const QImage image  = frameSpy.last().at(0).value<QImage>();
    const QSize  source = frameSpy.last().at(2).value<QSize>();

    QCOMPARE(image.size(), QSize(kWidth / 4, kHeight / 4));  // 缩到框内
    QCOMPARE(source, QSize(kWidth, kHeight));                // 真尺寸口径不变

    qInfo("e2e budget: image %dx%d but source %dx%d",
          image.width(), image.height(), source.width(), source.height());
}

// 码流分辨率变化 → 客户端必须重建解码线程（旧 worker 内的 MFT 有线程亲和性，
// 不能复用，只能停掉重建）。重建后新尺寸的帧要能正常出来。
void TestClientConnectionH264::resolutionChangeRebuildsDecoder()
{
    // 两个尺寸都取 16 的整数倍 —— H.264 的编码尺寸按宏块对齐，非对齐尺寸
    // （如 160x120 的高度）会让解码器报出补齐后的尺寸，干扰断言的读数。
    constexpr int kSmallW = 320, kSmallH = 240;
    constexpr int kLargeW = 640, kLargeH = 480;

    const QVector<QByteArray> small = encodeStream(kSmallW, kSmallH);
    const QVector<QByteArray> large = encodeStream(kLargeW, kLargeH);
    if (small.isEmpty() || large.isEmpty())
        QSKIP("Media Foundation H.264 编码器不可用（无 MFT / Windows N），跳过");

    E2E e2e;
    // 本用例测的是「分辨率变化要重建」，不是节流 —— 把重建间隔下限关掉，
    // 否则新尺寸那一帧会被节流挡掉（那正是 sizeFlipIsThrottled 管的事）。
    e2e.client.setDecoderRebuildIntervalMs(0);
    QVERIFY2(e2e.start(), "handshake with local HostServer failed");

    lanrdp::Session *srv = e2e.serverSession();
    QVERIFY2(srv != nullptr, "server-side session not found");

    // 先发小尺寸
    QSignalSpy spy(&e2e.client, &lanrdp::ClientConnection::frameReady);
    LogCapture log(&e2e.client);
    for (const QByteArray &payload : small)
        srv->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::ScreenFrameH264), payload);
    QVERIFY2(pumpUntil([&spy]() {
                 return spy.count() > 0 && spy.last().at(2).value<QSize>() == QSize(kSmallW, kSmallH);
             }),
             qPrintable(QStringLiteral("small-resolution frames did not arrive (got %1 frames, last source %2)")
                            .arg(spy.count())
                            .arg(spy.count() > 0 ? spy.last().at(2).value<QSize>().width() : -1)));

    // 再发大尺寸 —— 应触发解码线程重建
    const int before = spy.count();
    for (const QByteArray &payload : large)
        srv->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::ScreenFrameH264), payload);

    QVERIFY2(pumpUntil([&spy]() {
                 return spy.count() > 0 && spy.last().at(2).value<QSize>() == QSize(kLargeW, kLargeH);
             }),
             qPrintable(QStringLiteral("frames after resolution change did not arrive at the new size "
                                       "(got %1 frames, last source %2x%3)")
                            .arg(spy.count())
                            .arg(spy.last().at(2).value<QSize>().width())
                            .arg(spy.last().at(2).value<QSize>().height())));

    QVERIFY(spy.count() > before);
    // 重建会再打一条「解码线程就绪」（新 worker 起来了）
    QVERIFY2(log.contains(QStringLiteral("画面解码就绪")),
             qPrintable(log.lines.join(QLatin1Char('\n'))));

    qInfo("e2e resolution change: %dx%d -> %dx%d ok", kSmallW, kSmallH, kLargeW, kLargeH);
}

// 对端逐帧翻转分辨率时，不能被牵着「每帧重建一次解码线程」。重建是同步重操作
// （停旧线程 + 起新线程 + 等 MFT 就绪），全在 UI 线程 —— 每帧一次 ＝ 对端一个廉价 DoS。
// 这里验两条守则：
//   ① 重建有**最小间隔**：间隔内的尺寸变化一律丢弃，哪怕对方每轮都带关键帧；
//   ② 间隔过去后，**关键帧**仍能把新尺寸应用上（不是把新尺寸永久锁死）。
void TestClientConnectionH264::sizeFlipIsThrottled()
{
    constexpr int kSmallW = 320, kSmallH = 240;
    constexpr int kLargeW = 640, kLargeH = 480;

    const QVector<QByteArray> small = encodeStream(kSmallW, kSmallH);
    const QVector<QByteArray> large = encodeStream(kLargeW, kLargeH);
    if (small.isEmpty() || large.isEmpty())
        QSKIP("Media Foundation H.264 编码器不可用（无 MFT / Windows N），跳过");

    E2E e2e;
    // 重建间隔下限设得比「发抖动帧 -> 处理完」的耗时宽裕得多，用例本身不跟时间抢。
    e2e.client.setDecoderRebuildIntervalMs(1500);
    QVERIFY2(e2e.start(), "handshake with local HostServer failed");

    lanrdp::Session *srv = e2e.serverSession();
    QVERIFY2(srv != nullptr, "server-side session not found");

    QSignalSpy spy(&e2e.client, &lanrdp::ClientConnection::frameReady);
    LogCapture log(&e2e.client);

    // 1) 先建起小尺寸解码线程 —— 这条就是节流的时间基准
    for (const QByteArray &payload : small)
        srv->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::ScreenFrameH264), payload);
    QVERIFY2(pumpUntil([&spy]() {
                 return spy.count() > 0 && spy.last().at(2).value<QSize>() == QSize(kSmallW, kSmallH);
             }),
             "small-resolution frames did not arrive");
    QCOMPARE(log.count(QStringLiteral("画面解码就绪")), 1);

    // 2) 紧接着反复投递大尺寸（每轮都含关键帧）—— 间隔没到，应一帧都不重建
    for (int round = 0; round < 8; ++round)
        for (const QByteArray &payload : large)
            srv->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::ScreenFrameH264), payload);
    pumpUntil([]() { return false; }, 300);   // 把这些帧都走一遍

    QCOMPARE(log.count(QStringLiteral("画面解码就绪")), 1);   // 关键断言：一次重建都没有
    QCOMPARE(spy.last().at(2).value<QSize>(), QSize(kSmallW, kSmallH));  // 也没有大尺寸的帧漏出来

    // 3) 等过重建间隔后，关键帧应把新尺寸应用上（说明是「节流」不是「锁死」）
    QTest::qWait(1600);
    for (const QByteArray &payload : large)
        srv->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::ScreenFrameH264), payload);

    QVERIFY2(pumpUntil([&spy]() {
                 return spy.count() > 0 && spy.last().at(2).value<QSize>() == QSize(kLargeW, kLargeH);
             }),
             qPrintable(QStringLiteral("resolution change was never applied after the interval "
                                       "(got %1 frames, last source %2x%3)")
                            .arg(spy.count())
                            .arg(spy.count() > 0 ? spy.last().at(2).value<QSize>().width() : -1)
                            .arg(spy.count() > 0 ? spy.last().at(2).value<QSize>().height() : -1)));
    QCOMPARE(log.count(QStringLiteral("画面解码就绪")), 2);   // 恰好重建了一次

    qInfo("e2e throttle: size flips ignored within interval, applied on keyframe after it");
}

// 心跳只能由各自定时器**单向**驱动，不能「收到就回」。
//
// 病灶：两端都写了「收到 Heartbeat 立刻回一帧」。因为它们各自也有 3 s 定时器在发，
// 这一回帧纯属多余；但当两端都这么写，就变成无限乒乓 —— 轻量心跳退化成帧风暴
//（真机实测 42 s 收到 56711 帧，被控端 GUI 线程 CPU 从 35% 涨到 82%）。
// 这里让服务端把心跳周期压到 30 ms 猛发：客户端若回帧，帧数会跟着涨；不回则只有
// 它自己那份 3 s 周期的心跳。
void TestClientConnectionH264::heartbeatDoesNotPingPong()
{
    E2E e2e;
    e2e.host.setHeartbeatIntervalMs(30);   // 猛发：被控端侧的正常值是 3000
    e2e.host.setDeadTimeoutMs(5000);       // 观测期内别把客户端判死踢掉
    QVERIFY2(e2e.start(), "handshake with local HostServer failed");

    lanrdp::Session *srv = e2e.serverSession();
    QVERIFY2(srv != nullptr, "server-side session not found");

    // 统计「客户端发回被控端」的心跳帧数（同线程直连，不必注册 Frame 元类型）
    int fromClient = 0;
    QObject::connect(srv, &lanrdp::Session::frameReceived, srv,
                     [&fromClient](const lanrdp::Frame &f) {
                         if (f.type == static_cast<std::uint8_t>(lanrdp::FrameType::Heartbeat))
                             ++fromClient;
                     });

    QTest::qWait(600);   // 30 ms 周期 ⇒ 服务端约发 20 帧

    // 客户端自己的定时器是 3 s ⇒ 这 600 ms 内期望 0 帧；回帧则会是 ~20 帧。
    QVERIFY2(fromClient < 5,
             qPrintable(QStringLiteral("客户端对心跳回了 %1 帧 —— 心跳正在乒乓").arg(fromClient)));

    qInfo("heartbeat frames from client in 600 ms: %d", fromClient);
}

QTEST_GUILESS_MAIN(TestClientConnectionH264)
#include "test_clientconnection_h264.moc"
