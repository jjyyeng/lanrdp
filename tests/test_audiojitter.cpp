#include <QtTest/QtTest>

#include "protocol/AudioChunk.h"
#include "viewer/AudioJitterBuffer.h"

// 播放端抖动缓冲的策略：**全部由缓冲字节深度决定，不读任何时钟**。
//
// 之所以要单独成类并单测：这是整条音频链路里唯一一处「策略」，而它同时被
// 两个线程碰（UI 线程 push、播放线程 take）。策略写错的表现是「越听越滞后」
// 或「一直咔嗒」，靠人耳很难定位到具体分支 —— 所以喂确定的数据、断言确定的
// 出参，而不是靠真机听。
class TestAudioJitter : public QObject
{
    Q_OBJECT

private slots:
    void emptyBufferReturnsSilence();
    void oneBlockIsReturnedIntact();
    void partialTakeKeepsRemainder();
    void overflowDropsDownToTarget();
    void underrunPadsAndCountsPerTake();
    void clearResetsBufferAndStats();
    void configureRecomputesLimits();
    void takeStaysFrameAligned();
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

// 48000 Hz × 4 B × 20 ms = 3840 B / 块
constexpr qsizetype kBlock = 3840;

QByteArray makePcm(int blocks, char fill)
{
    return QByteArray(kBlock * blocks, fill);
}

bool isAllZero(const QByteArray &data)
{
    for (char c : data)
    {
        if (c != 0)
            return false;
    }
    return true;
}

} // namespace

void TestAudioJitter::emptyBufferReturnsSilence()
{
    lanrdp::AudioJitterBuffer buf;
    buf.configure(stereo48k());

    const auto r = buf.take(kBlock);
    QCOMPARE(r.data.size(), kBlock);
    QVERIFY(isAllZero(r.data));
    QVERIFY(r.underrun);
    QCOMPARE(buf.underruns(), quint64(1));
    QCOMPARE(buf.bufferedBytes(), qsizetype(0));
}

void TestAudioJitter::oneBlockIsReturnedIntact()
{
    lanrdp::AudioJitterBuffer buf;
    buf.configure(stereo48k());

    QByteArray block = makePcm(1, '\x11');
    buf.push(block);
    QCOMPARE(buf.bufferedBytes(), kBlock);

    const auto r = buf.take(kBlock);
    QCOMPARE(r.data, block);
    QVERIFY(!r.underrun);
    QCOMPARE(buf.bufferedBytes(), qsizetype(0));
    QCOMPARE(buf.droppedBytes(), qsizetype(0));
}

void TestAudioJitter::partialTakeKeepsRemainder()
{
    lanrdp::AudioJitterBuffer buf;
    buf.configure(stereo48k());

    QByteArray block(kBlock, '\0');
    for (int i = 0; i < kBlock; ++i)
        block[i] = char(i % 251);
    buf.push(block);

    const auto first = buf.take(1000);
    QCOMPARE(first.data.size(), 1000);
    QCOMPARE(first.data, block.left(1000));
    QCOMPARE(buf.bufferedBytes(), kBlock - 1000);

    const auto second = buf.take(kBlock - 1000);
    QCOMPARE(second.data, block.mid(1000));
    QCOMPARE(buf.bufferedBytes(), qsizetype(0));
}

void TestAudioJitter::overflowDropsDownToTarget()
{
    lanrdp::AudioJitterBuffer buf;
    buf.configure(stereo48k());

    // 上限 200 ms = 10 块。一次塞 12 块（模拟「对端补发一波」或 UI 线程被卡住）
    QByteArray burst;
    for (int i = 0; i < 12; ++i)
        burst.append(makePcm(1, char('a' + i)));
    buf.push(burst);

    // 丢到目标深度 60 ms = 3 块，而不是「只丢到刚好不超上限」——
    // 后者会把延迟永久钉在 200 ms 上，越听越滞后
    QCOMPARE(buf.bufferedBytes(), kBlock * 3);
    QCOMPARE(buf.droppedBytes(), kBlock * 9);

    // 留下的是**最新**的三块
    const auto r = buf.take(kBlock * 3);
    QCOMPARE(r.data.left(kBlock), makePcm(1, char('a' + 9)));
    QCOMPARE(r.data.mid(kBlock, kBlock), makePcm(1, char('a' + 10)));
    QCOMPARE(r.data.right(kBlock), makePcm(1, char('a' + 11)));
    QVERIFY(!r.underrun);
}

void TestAudioJitter::underrunPadsAndCountsPerTake()
{
    lanrdp::AudioJitterBuffer buf;
    buf.configure(stereo48k());

    buf.push(makePcm(1, '\x22'));
    const auto r = buf.take(kBlock * 2);   // 要 2 块，只有 1 块
    QCOMPARE(r.data.size(), kBlock * 2);
    QCOMPARE(r.data.left(kBlock), makePcm(1, '\x22'));
    QVERIFY(isAllZero(r.data.mid(kBlock)));
    QVERIFY(r.underrun);
    QCOMPARE(buf.underruns(), quint64(1));

    // 每「一次取不满」计一次（设备每次回调算一拍）
    buf.take(kBlock);
    QCOMPARE(buf.underruns(), quint64(2));
}

void TestAudioJitter::clearResetsBufferAndStats()
{
    lanrdp::AudioJitterBuffer buf;
    buf.configure(stereo48k());

    buf.push(makePcm(12, 'x'));
    buf.take(kBlock * 100);

    QVERIFY(buf.droppedBytes() > 0);

    buf.clear();
    QCOMPARE(buf.bufferedBytes(), qsizetype(0));
    QCOMPARE(buf.droppedBytes(), qsizetype(0));
    QCOMPARE(buf.underruns(), quint64(0));
    QVERIFY(buf.isEmpty());

    // 清空之后仍然可用（切格 / 关开关都走这条路，不能把对象弄成一次性的）
    buf.push(makePcm(1, '\x33'));
    QCOMPARE(buf.take(kBlock).data, makePcm(1, '\x33'));
}

void TestAudioJitter::configureRecomputesLimits()
{
    lanrdp::AudioJitterBuffer buf;
    buf.configure(stereo48k());
    buf.push(makePcm(1, 'y'));
    QCOMPARE(buf.bufferedBytes(), kBlock);

    // 换设备：44.1 kHz 单声道 ⇒ 20 ms = 882 帧 × 2 B = 1764 B
    lanrdp::AudioFormat mono44;
    mono44.sampleRate = 44100;
    mono44.channels   = 1;
    buf.configure(mono44);

    QCOMPARE(buf.bufferedBytes(), qsizetype(0));   // 旧格式的数据不能留在缓冲里
    buf.push(QByteArray(1764, '\x44'));
    QCOMPARE(buf.take(1764).data, QByteArray(1764, '\x44'));
}

void TestAudioJitter::takeStaysFrameAligned()
{
    lanrdp::AudioJitterBuffer buf;
    buf.configure(stereo48k());

    QByteArray block(kBlock, '\x55');
    buf.push(block);
    buf.push(block);

    // 1002 不是 4（立体声 16 位 = 每帧 4 B）的整数倍 ⇒ 向上取整到 1004。
    // 半帧数据交给设备就是错位噪声，宁可少写半个采样点。
    const auto r = buf.take(1002);
    QCOMPARE(r.data.size(), 1004);
    QCOMPARE(r.data.size() % 4, 0);
}

QTEST_APPLESS_MAIN(TestAudioJitter)
#include "test_audiojitter.moc"
