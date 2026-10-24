#include <QtTest/QtTest>

#include <QByteArray>

#include "protocol/H264Frame.h"

// SCREEN_FRAME_H264 载荷编解码
//
// 载荷布局：[width u32 LE][height u32 LE][flags u8][Annex-B 码流...]
// flags 的 bit0 表示关键帧。码流长度不需要单独存——外层 Frame 的 PayloadLen
// 已经精确定界，本层只需防御头本身非法的情况。
class TestH264Frame : public QObject
{
    Q_OBJECT

private slots:
    void roundTripPreservesMetadataAndBitstream();
    void keyframeFlagIsCarriedInBothDirections();
    void decodeRejectsBogusPayload();
    void encodeRejectsInvalidInput();
};

namespace {

// 构造一段假的 Annex-B 码流：起始码 + SPS + PPS + IDR
QByteArray makeKeyframeBitstream()
{
    QByteArray data;
    data.append("\x00\x00\x00\x01", 4);
    data.append("\x67\x64\x00\x28", 4);          // SPS
    data.append("\x00\x00\x00\x01", 4);
    data.append("\x68\xee\x3c\x80", 4);          // PPS
    data.append("\x00\x00\x00\x01", 4);
    data.append("\x65\x88\x84", 3);              // IDR
    return data;
}

} // namespace

void TestH264Frame::roundTripPreservesMetadataAndBitstream()
{
    lanrdp::H264Frame frame;
    frame.width     = 1920;
    frame.height    = 1080;
    frame.keyframe  = true;
    frame.bitstream = makeKeyframeBitstream();

    const QByteArray payload = lanrdp::encodeH264Frame(frame);
    QVERIFY(!payload.isEmpty());
    QCOMPARE(payload.size(), lanrdp::kH264FrameHeaderSize + frame.bitstream.size());

    const auto decoded = lanrdp::decodeH264Frame(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->width, 1920u);
    QCOMPARE(decoded->height, 1080u);
    QVERIFY(decoded->keyframe);
    QCOMPARE(decoded->bitstream, frame.bitstream);
}

void TestH264Frame::keyframeFlagIsCarriedInBothDirections()
{
    for (const bool keyframe : {false, true}) {
        lanrdp::H264Frame frame;
        frame.width     = 640;
        frame.height    = 360;
        frame.keyframe  = keyframe;
        frame.bitstream = QByteArray("\x00\x00\x00\x01\x41\x9a", 6);  // 非 IDR 片

        const auto decoded = lanrdp::decodeH264Frame(lanrdp::encodeH264Frame(frame));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->keyframe, keyframe);
    }
}

void TestH264Frame::decodeRejectsBogusPayload()
{
    const QByteArray good = lanrdp::encodeH264Frame(
        lanrdp::H264Frame{1920, 1080, true, makeKeyframeBitstream()});
    QVERIFY(!good.isEmpty());

    // 比头还短
    QVERIFY(!lanrdp::decodeH264Frame(QByteArray()).has_value());
    QVERIFY(!lanrdp::decodeH264Frame(QByteArray(lanrdp::kH264FrameHeaderSize - 1, '\0')).has_value());

    // 只有头、没有码流
    QVERIFY(!lanrdp::decodeH264Frame(QByteArray(lanrdp::kH264FrameHeaderSize, '\0')).has_value());

    // 尺寸为 0
    QByteArray zeroDims = good;
    zeroDims[0] = '\0';
    zeroDims[1] = '\0';
    zeroDims[2] = '\0';
    zeroDims[3] = '\0';
    QVERIFY(!lanrdp::decodeH264Frame(zeroDims).has_value());

    // 尺寸被伪造成 65536（超过 8192 上限），防止骗出巨额分配
    QByteArray huge = good;
    huge[0] = '\x00';
    huge[1] = '\x00';
    huge[2] = '\x01';
    huge[3] = '\x00';  // width = 0x00010000 = 65536
    QVERIFY(!lanrdp::decodeH264Frame(huge).has_value());
}

void TestH264Frame::encodeRejectsInvalidInput()
{
    lanrdp::H264Frame noStream;
    noStream.width  = 1920;
    noStream.height = 1080;
    QVERIFY(lanrdp::encodeH264Frame(noStream).isEmpty());

    lanrdp::H264Frame noSize;
    noSize.bitstream = makeKeyframeBitstream();
    QVERIFY(lanrdp::encodeH264Frame(noSize).isEmpty());
}

QTEST_GUILESS_MAIN(TestH264Frame)
#include "test_h264frame.moc"
