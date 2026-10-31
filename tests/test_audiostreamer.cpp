#include <QtTest/QtTest>

#include <QTcpServer>
#include <QTcpSocket>

#include "host/AudioStreamer.h"
#include "net/Session.h"
#include "protocol/Frame.h"

// 被控端音频扇出：目标管理 / 格式补发 / 逐目标背压。
//
// 全部走**真实 loopback**（与 test_streamer 同一套写法），不做 Session 打桩 ——
// 打桩会把「背压读的是 socket 的 pendingBytes」这条最容易写错的口径盖掉。
class TestAudioStreamer : public QObject
{
    Q_OBJECT

private slots:
    void configThenDataReachesTarget();
    void secondTargetGetsItsOwnConfig();
    void formatIsLearnedWithoutTargets();
    void formatChangeResendsConfig();
    void backpressureSkipsOnlyThatTarget();
    void removedTargetStopsReceiving();
    void emptyPcmIsIgnored();
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

QByteArray pcmBlock(char fill)
{
    return QByteArray(3840, fill);
}

// 一条 loopback 连接的被控端侧 socket（把它交给 Session 当推送目标）。
struct Link
{
    QTcpServer  server;
    QTcpSocket  client;
    QTcpSocket *serverSide = nullptr;
    QByteArray  received;

    bool open()
    {
        if (!server.listen(QHostAddress::LocalHost, 0))
            return false;
        client.connectToHost(QHostAddress::LocalHost, server.serverPort());
        if (!client.waitForConnected(3000))
            return false;
        if (!server.waitForNewConnection(3000))
            return false;
        serverSide = server.nextPendingConnection();
        if (!serverSide)
            return false;
        QObject::connect(&client, &QTcpSocket::readyRead, [this]() { received.append(client.readAll()); });
        return true;
    }
};

// 把已收到的字节解析成帧序列（不完整的尾巴留着）。
QVector<lanrdp::Frame> parseAll(QByteArray *buffer)
{
    QVector<lanrdp::Frame> frames;
    qsizetype              offset = 0;
    for (;;)
    {
        if (offset >= buffer->size())
            break;
        lanrdp::Frame frame;
        qsizetype     consumed = 0;
        if (lanrdp::Frame::tryParse(*buffer, &frame, &consumed, offset) != lanrdp::ParseStatus::Ok)
            break;
        frames.append(frame);
        offset += consumed;
    }
    if (offset > 0)
        buffer->remove(0, offset);
    return frames;
}

} // namespace

void TestAudioStreamer::configThenDataReachesTarget()
{
    Link link;
    QVERIFY(link.open());
    lanrdp::Session session(link.serverSide);

    lanrdp::AudioStreamer streamer;
    streamer.addTarget(&session);
    QCOMPARE(streamer.targetCount(), 1);

    streamer.submit(stereo48k(), 100, pcmBlock('\x01'));
    QCOMPARE(streamer.chunksSent(), quint64(1));

    QTRY_VERIFY_WITH_TIMEOUT(link.received.size() >= 8 + 9 + 8 + 3840, 5000);
    const auto frames = parseAll(&link.received);
    QCOMPARE(frames.size(), 2);

    QCOMPARE(frames.at(0).type, static_cast<quint8>(lanrdp::FrameType::AudioConfig));
    const auto fmt = lanrdp::decodeAudioConfig(frames.at(0).payload);
    QVERIFY(fmt.has_value());
    QCOMPARE(fmt->sampleRate, quint32(48000));
    QCOMPARE(fmt->channels, quint8(2));

    QCOMPARE(frames.at(1).type, static_cast<quint8>(lanrdp::FrameType::AudioData));
    const auto chunk = lanrdp::decodeAudioData(frames.at(1).payload);
    QVERIFY(chunk.has_value());
    QCOMPARE(chunk->tsMs, qint64(100));
    QCOMPARE(chunk->pcm, pcmBlock('\x01'));

    // 第二块不再重复发配置
    streamer.submit(stereo48k(), 120, pcmBlock('\x02'));
    QTRY_VERIFY_WITH_TIMEOUT(link.received.size() >= 8 + 3840, 5000);
    const auto more = parseAll(&link.received);
    QCOMPARE(more.size(), 1);
    QCOMPARE(more.at(0).type, static_cast<quint8>(lanrdp::FrameType::AudioData));
}

void TestAudioStreamer::secondTargetGetsItsOwnConfig()
{
    Link a;
    Link b;
    QVERIFY(a.open());
    QVERIFY(b.open());
    lanrdp::Session sessionA(a.serverSide);
    lanrdp::Session sessionB(b.serverSide);

    lanrdp::AudioStreamer streamer;
    streamer.addTarget(&sessionA);
    streamer.submit(stereo48k(), 0, pcmBlock('\x03'));

    // 第二个目标**中途**接入：必须自己收到一帧配置，而不是等格式变化
    streamer.addTarget(&sessionB);
    QTRY_VERIFY_WITH_TIMEOUT(b.received.size() >= 8 + 9, 5000);
    const auto bFrames = parseAll(&b.received);
    QCOMPARE(bFrames.size(), 1);
    QCOMPARE(bFrames.at(0).type, static_cast<quint8>(lanrdp::FrameType::AudioConfig));

    streamer.submit(stereo48k(), 20, pcmBlock('\x04'));
    QTRY_VERIFY_WITH_TIMEOUT(b.received.size() >= 8 + 3840, 5000);
    const auto bMore = parseAll(&b.received);
    QCOMPARE(bMore.size(), 1);
    QCOMPARE(bMore.at(0).type, static_cast<quint8>(lanrdp::FrameType::AudioData));
}

void TestAudioStreamer::formatIsLearnedWithoutTargets()
{
    lanrdp::AudioStreamer streamer;
    streamer.submit(stereo48k(), 0, pcmBlock('\x05'));
    QVERIFY(streamer.hasFormat());
    QCOMPARE(streamer.chunksSent(), quint64(0));   // 没人看，一个字节都不发

    Link link;
    QVERIFY(link.open());
    lanrdp::Session session(link.serverSide);
    streamer.addTarget(&session);

    QTRY_VERIFY_WITH_TIMEOUT(link.received.size() >= 8 + 9, 5000);
    const auto frames = parseAll(&link.received);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.at(0).type, static_cast<quint8>(lanrdp::FrameType::AudioConfig));
}

void TestAudioStreamer::formatChangeResendsConfig()
{
    Link link;
    QVERIFY(link.open());
    lanrdp::Session session(link.serverSide);

    lanrdp::AudioStreamer streamer;
    streamer.addTarget(&session);
    streamer.submit(stereo48k(), 0, pcmBlock('\x06'));

    lanrdp::AudioFormat mono44 = stereo48k();
    mono44.sampleRate = 44100;
    mono44.channels   = 1;
    streamer.submit(mono44, 20, QByteArray(1764, '\x07'));

    QTRY_VERIFY_WITH_TIMEOUT(link.received.size() >= (8 + 9 + 8 + 3840) + (8 + 9 + 8 + 1764), 5000);
    const auto frames = parseAll(&link.received);
    QCOMPARE(frames.size(), 4);
    QCOMPARE(frames.at(0).type, static_cast<quint8>(lanrdp::FrameType::AudioConfig));
    QCOMPARE(frames.at(1).type, static_cast<quint8>(lanrdp::FrameType::AudioData));
    QCOMPARE(frames.at(2).type, static_cast<quint8>(lanrdp::FrameType::AudioConfig));
    QCOMPARE(frames.at(3).type, static_cast<quint8>(lanrdp::FrameType::AudioData));

    const auto fmt = lanrdp::decodeAudioConfig(frames.at(2).payload);
    QVERIFY(fmt.has_value());
    QCOMPARE(fmt->sampleRate, quint32(44100));
    QCOMPARE(fmt->channels, quint8(1));
}

void TestAudioStreamer::backpressureSkipsOnlyThatTarget()
{
    Link slowLink;
    Link fastLink;
    QVERIFY(slowLink.open());
    QVERIFY(fastLink.open());
    lanrdp::Session slow(slowLink.serverSide);
    lanrdp::Session fast(fastLink.serverSide);

    lanrdp::AudioStreamer streamer;
    streamer.setBackpressureBytes(1000);
    // 慢目标积压 5000 B（超过水位），快目标空。
    // 注入口径而不是真把 socket 塞满：真塞 1 MiB 会让用例退化成「等 TCP 窗口」，很脆。
    streamer.setPendingSource([&slow](lanrdp::Session *s) { return s == &slow ? qint64(5000) : qint64(0); });

    streamer.addTarget(&slow);
    streamer.addTarget(&fast);
    streamer.submit(stereo48k(), 0, pcmBlock('\x08'));

    QCOMPARE(streamer.chunksSent(), quint64(1));
    QCOMPARE(streamer.chunksSkipped(), quint64(1));

    // 配置帧不受背压约束（9 字节，且没有它渲染端就起不来）：两个目标都收到了
    QTRY_VERIFY_WITH_TIMEOUT(slowLink.received.size() >= 8 + 9, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(fastLink.received.size() >= 8 + 9 + 8 + 3840, 5000);

    const auto slowFrames = parseAll(&slowLink.received);
    QCOMPARE(slowFrames.size(), 1);
    QCOMPARE(slowFrames.at(0).type, static_cast<quint8>(lanrdp::FrameType::AudioConfig));

    const auto fastFrames = parseAll(&fastLink.received);
    QCOMPARE(fastFrames.size(), 2);
    QCOMPARE(fastFrames.at(1).type, static_cast<quint8>(lanrdp::FrameType::AudioData));

    // 积压恢复后立刻继续发（不是永久拉黑）
    streamer.setPendingSource([](lanrdp::Session *) { return qint64(0); });
    streamer.submit(stereo48k(), 20, pcmBlock('\x09'));
    QCOMPARE(streamer.chunksSent(), quint64(3));
}

void TestAudioStreamer::removedTargetStopsReceiving()
{
    Link link;
    QVERIFY(link.open());
    lanrdp::Session session(link.serverSide);

    lanrdp::AudioStreamer streamer;
    streamer.addTarget(&session);
    streamer.submit(stereo48k(), 0, pcmBlock('\x0a'));
    QTRY_VERIFY_WITH_TIMEOUT(link.received.size() >= 8 + 9 + 8 + 3840, 5000);
    link.received.clear();

    streamer.removeTarget(&session);
    QCOMPARE(streamer.targetCount(), 0);
    streamer.submit(stereo48k(), 20, pcmBlock('\x0b'));

    QTest::qWait(120);
    QVERIFY(link.received.isEmpty());

    // clearTargets 是停机路径，幂等
    streamer.clearTargets();
    QCOMPARE(streamer.targetCount(), 0);
}

void TestAudioStreamer::emptyPcmIsIgnored()
{
    Link link;
    QVERIFY(link.open());
    lanrdp::Session session(link.serverSide);

    lanrdp::AudioStreamer streamer;
    streamer.addTarget(&session);

    streamer.submit(stereo48k(), 0, QByteArray());
    QCOMPARE(streamer.chunksSent(), quint64(0));
    QVERIFY(!streamer.hasFormat());

    // 非法格式同样不学（否则会给对端一个解不了的配置）
    lanrdp::AudioFormat bad = stereo48k();
    bad.sampleRate = 0;
    streamer.submit(bad, 0, pcmBlock('\x0c'));
    QCOMPARE(streamer.chunksSent(), quint64(0));
    QVERIFY(!streamer.hasFormat());

    QTest::qWait(80);
    QVERIFY(link.received.isEmpty());
}

QTEST_GUILESS_MAIN(TestAudioStreamer)
#include "test_audiostreamer.moc"
