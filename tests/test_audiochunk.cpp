#include <QtTest/QtTest>

#include "protocol/AudioChunk.h"

// 音频两帧的线上格式（AUDIO_CONFIG / AUDIO_DATA）与定长块装配器。
//
// 这一层的价值全在「坏帧不许进播放链路」：采样率 / 声道是解码侧拿去分配
// 缓冲的字段，长度字段来自线上 ⇒ 越界、截断、奇数长度都必须回 nullopt，
// 让上层静默丢一块（音频坏一帧不值得断连）。
class TestAudioChunk : public QObject
{
    Q_OBJECT

private slots:
    // ---- AUDIO_CONFIG ----
    void configRoundTrips();
    void configRejectsBadVersion();
    void configRejectsZeroRate();
    void configRejectsTooManyChannels();
    void configRejectsNonS16BitDepth();
    void configRejectsTruncated();

    // ---- 格式校验与块尺寸 ----
    void blockBytesMatchesFormat();
    void blockBytesFollowsSampleRate();
    void invalidFormatsAreRejected();

    // ---- AUDIO_DATA ----
    void dataRoundTrips();
    void dataRejectsShortHeader();
    void dataRejectsOddPayload();
    void dataRejectsPartialFrame();

    // ---- 定长块装配 ----
    void assemblerEmitsExactBlocks();
    void assemblerKeepsPartialTail();
    void assemblerClearDropsTail();
    void assemblerHandlesLargeAppend();
    void assemblerIgnoresGarbageArguments();
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

} // namespace

void TestAudioChunk::configRoundTrips()
{
    const lanrdp::AudioFormat in = stereo48k();

    const QByteArray payload = lanrdp::encodeAudioConfig(in);
    QCOMPARE(payload.size(), 9);

    const auto out = lanrdp::decodeAudioConfig(payload);
    QVERIFY(out.has_value());
    QCOMPARE(out->sampleRate, quint32(48000));
    QCOMPARE(out->channels, quint8(2));
    QCOMPARE(out->bitsPerSample, quint8(16));
    QCOMPARE(out->blockMs, quint16(20));
}

void TestAudioChunk::configRejectsBadVersion()
{
    QByteArray payload = lanrdp::encodeAudioConfig(stereo48k());
    payload[0] = char(99);
    QVERIFY(!lanrdp::decodeAudioConfig(payload).has_value());
}

void TestAudioChunk::configRejectsZeroRate()
{
    lanrdp::AudioFormat f = stereo48k();
    f.sampleRate = 0;
    // 编码侧不拦（它只负责写字节），解码侧必须拒 —— 线上字段一律不可信
    QVERIFY(!lanrdp::decodeAudioConfig(lanrdp::encodeAudioConfig(f)).has_value());
}

void TestAudioChunk::configRejectsTooManyChannels()
{
    lanrdp::AudioFormat f = stereo48k();
    f.channels = lanrdp::kAudioMaxChannels + 1;
    QVERIFY(!lanrdp::decodeAudioConfig(lanrdp::encodeAudioConfig(f)).has_value());
}

void TestAudioChunk::configRejectsNonS16BitDepth()
{
    lanrdp::AudioFormat f = stereo48k();
    f.bitsPerSample = 24;
    QVERIFY(!lanrdp::decodeAudioConfig(lanrdp::encodeAudioConfig(f)).has_value());
}

void TestAudioChunk::configRejectsTruncated()
{
    const QByteArray payload = lanrdp::encodeAudioConfig(stereo48k());
    QVERIFY(!lanrdp::decodeAudioConfig(payload.left(8)).has_value());
    QVERIFY(!lanrdp::decodeAudioConfig(QByteArray()).has_value());
}

void TestAudioChunk::blockBytesMatchesFormat()
{
    // 20 ms @48k = 960 帧；立体声 16 位 = 每帧 4 B ⇒ 3840 B
    QCOMPARE(lanrdp::audioBlockBytes(stereo48k()), qint64(3840));
}

void TestAudioChunk::blockBytesFollowsSampleRate()
{
    lanrdp::AudioFormat f = stereo48k();
    f.sampleRate = 44100;   // 蓝牙耳机的常见口径
    QCOMPARE(lanrdp::audioBlockBytes(f), qint64(3528));

    f.channels = 1;
    QCOMPARE(lanrdp::audioBlockBytes(f), qint64(1764));
}

void TestAudioChunk::invalidFormatsAreRejected()
{
    QVERIFY(lanrdp::isValidAudioFormat(stereo48k()));

    lanrdp::AudioFormat f = stereo48k();
    f.channels = 0;
    QVERIFY(!lanrdp::isValidAudioFormat(f));

    f = stereo48k();
    f.blockMs = 0;
    QVERIFY(!lanrdp::isValidAudioFormat(f));

    f = stereo48k();
    f.blockMs = lanrdp::kAudioMaxBlockMs + 1;
    QVERIFY(!lanrdp::isValidAudioFormat(f));

    f = stereo48k();
    f.sampleRate = lanrdp::kAudioMaxSampleRate + 1;
    QVERIFY(!lanrdp::isValidAudioFormat(f));
}

void TestAudioChunk::dataRoundTrips()
{
    QByteArray pcm(3840, '\0');
    for (int i = 0; i < pcm.size(); ++i)
        pcm[i] = char(i % 251);

    const QByteArray payload = lanrdp::encodeAudioData(1234, pcm);
    QCOMPARE(payload.size(), 8 + pcm.size());

    const auto chunk = lanrdp::decodeAudioData(payload);
    QVERIFY(chunk.has_value());
    QCOMPARE(chunk->tsMs, qint64(1234));
    QCOMPARE(chunk->pcm, pcm);
}

void TestAudioChunk::dataRejectsShortHeader()
{
    QVERIFY(!lanrdp::decodeAudioData(QByteArray(7, '\0')).has_value());
    QVERIFY(!lanrdp::decodeAudioData(QByteArray()).has_value());
}

void TestAudioChunk::dataRejectsOddPayload()
{
    // 16 位交织 ⇒ PCM 长度必须是偶数。奇数说明边界已经错了，
    // 解出来是半字节错位的噪声，宁可丢。
    const QByteArray payload = lanrdp::encodeAudioData(0, QByteArray(3839, '\0'));
    QVERIFY(!lanrdp::decodeAudioData(payload).has_value());

    // 只有头、没有 PCM 也算合法（静音块被上层过滤，不必在这一层判死）
    const auto empty = lanrdp::decodeAudioData(lanrdp::encodeAudioData(7, QByteArray()));
    QVERIFY(empty.has_value());
    QVERIFY(empty->pcm.isEmpty());
}

void TestAudioChunk::dataRejectsPartialFrame()
{
    lanrdp::AudioFormat format;   // 48 kHz / 2 声道 / 16 位 ⇒ blockAlign = 4
    QCOMPARE(lanrdp::audioBlockAlign(format), qint64(4));

    // 整帧 ⇒ 收
    QVERIFY(lanrdp::decodeAudioData(lanrdp::encodeAudioData(0, QByteArray(3840, '\0')), &format)
                .has_value());

    // 多出 2 字节（半个采样）⇒ 拒。放过去的话播放端的字节队列会永久留着这个尾数，
    // 之后每取一块都整体平移半个采样 —— 越听越歪，而且不会自愈。
    QVERIFY(!lanrdp::decodeAudioData(lanrdp::encodeAudioData(0, QByteArray(3842, '\0')), &format)
                 .has_value());

    // 不知道格式时只能查偶数长度：旧行为不能被这条新规则改掉。
    QVERIFY(lanrdp::decodeAudioData(lanrdp::encodeAudioData(0, QByteArray(3842, '\0')))
                .has_value());
}

void TestAudioChunk::assemblerEmitsExactBlocks()
{
    lanrdp::AudioBlockAssembler asm_;
    asm_.setBlockBytes(3840);
    QCOMPARE(asm_.blockBytes(), qint64(3840));

    QByteArray one(3840, 'a');
    const auto blocks = asm_.append(one.constData(), one.size());
    QCOMPARE(blocks.size(), 1);
    QCOMPARE(blocks.at(0).size(), 3840);
    QCOMPARE(asm_.pendingBytes(), qint64(0));
}

void TestAudioChunk::assemblerKeepsPartialTail()
{
    lanrdp::AudioBlockAssembler asm_;
    asm_.setBlockBytes(3840);

    QByteArray data(3840 * 2 + 100, 'b');
    const auto blocks = asm_.append(data.constData(), data.size());
    QCOMPARE(blocks.size(), 2);
    QCOMPARE(asm_.pendingBytes(), qint64(100));

    // 再来一块不够的：仍然不出块
    QByteArray more(3839 - 100, 'c');
    QCOMPARE(asm_.append(more.constData(), more.size()).size(), 0);
    QCOMPARE(asm_.pendingBytes(), qint64(3839));

    QByteArray last(1, 'd');
    QCOMPARE(asm_.append(last.constData(), last.size()).size(), 1);
    QCOMPARE(asm_.pendingBytes(), qint64(0));
}

void TestAudioChunk::assemblerClearDropsTail()
{
    lanrdp::AudioBlockAssembler asm_;
    asm_.setBlockBytes(3840);

    QByteArray data(500, 'e');
    QCOMPARE(asm_.append(data.constData(), data.size()).size(), 0);
    QCOMPARE(asm_.pendingBytes(), qint64(500));

    // 不足一块的尾巴在停机时直接丢掉：连接正在关，多送 20 ms 换不来听感。
    asm_.clear();
    QCOMPARE(asm_.pendingBytes(), qint64(0));
}

void TestAudioChunk::assemblerHandlesLargeAppend()
{
    lanrdp::AudioBlockAssembler asm_;
    asm_.setBlockBytes(3840);

    QByteArray data(3840 * 5, 'f');
    const auto blocks = asm_.append(data.constData(), data.size());
    QCOMPARE(blocks.size(), 5);
    for (const QByteArray &b : blocks)
        QCOMPARE(b.size(), 3840);
}

void TestAudioChunk::assemblerIgnoresGarbageArguments()
{
    lanrdp::AudioBlockAssembler asm_;
    asm_.setBlockBytes(3840);

    QCOMPARE(asm_.append(nullptr, 100).size(), 0);
    QCOMPARE(asm_.append("abc", 0).size(), 0);
    QCOMPARE(asm_.append("abc", -5).size(), 0);
    QCOMPARE(asm_.pendingBytes(), qint64(0));

    // 块尺寸设成 0 / 负数 ⇒ 视为「未配置」，永不出块（而不是除零 / 无限循环）
    asm_.setBlockBytes(0);
    QCOMPARE(asm_.append("abcdef", 6).size(), 0);
    asm_.clear();
    QCOMPARE(asm_.pendingBytes(), qint64(0));
}

QTEST_APPLESS_MAIN(TestAudioChunk)
#include "test_audiochunk.moc"
