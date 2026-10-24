#include <QtTest/QtTest>

#include <QImage>

#include "protocol/ScreenFrame.h"

// SCREEN_FRAME_RAW 载荷编解码
class TestScreenFrame : public QObject
{
    Q_OBJECT

private slots:
    void roundTripPreservesEveryPixel();
    void rejectsTruncatedOrBogusPayload();
    void convertsNonArgb32Input();
    void rejectsAbsurdDimensionsBeforeAllocating();
    void rejectsPayloadTooSmallForDeclaredPixels();
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

} // namespace

void TestScreenFrame::roundTripPreservesEveryPixel()
{
    const QImage image = makeImage(37, 19);  // 非对齐尺寸，验证行跨距处理

    const QByteArray payload = lanrdp::encodeRawFrame(image);
    QVERIFY(!payload.isEmpty());
    QCOMPARE(payload.size(), lanrdp::kRawFrameHeaderSize + 37 * 19 * 4);

    const QImage decoded = lanrdp::decodeRawFrame(payload);
    QVERIFY(!decoded.isNull());
    QCOMPARE(decoded.size(), image.size());
    QCOMPARE(decoded, image);  // 逐像素比较
}

void TestScreenFrame::rejectsTruncatedOrBogusPayload()
{
    QByteArray payload = lanrdp::encodeRawFrame(makeImage(8, 8));
    QVERIFY(!payload.isEmpty());

    QByteArray truncated = payload;
    truncated.chop(10);
    QVERIFY(lanrdp::decodeRawFrame(truncated).isNull());

    QVERIFY(lanrdp::decodeRawFrame(QByteArray()).isNull());
    QVERIFY(lanrdp::decodeRawFrame(QByteArray(8, '\0')).isNull());  // width=height=0
}

void TestScreenFrame::convertsNonArgb32Input()
{
    QImage source(8, 8, QImage::Format_RGB32);
    source.fill(Qt::red);

    const QImage decoded = lanrdp::decodeRawFrame(lanrdp::encodeRawFrame(source));
    QCOMPARE(decoded.size(), QSize(8, 8));
    QCOMPARE(decoded.pixel(0, 0), qRgba(255, 0, 0, 255));
}

// 声明尺寸离谱（> 16384）时必须**在分配之前**就被拒 —— 否则一个几十字节的伪造载荷
// 就能骗出几 GB 的 QImage 分配（DoS）。
// 异常的宽高必须让解码**返回空图**（而不是按伪造尺寸去分配/越界读）。
//
// 注意这条锁的是「返回值契约」，不是某一条具体的守卫：尺寸守卫、长度守卫、
// 以及 QImage 构造失败的 isNull() 兜底，最终表现都是「空图」。
// 想定向验证「是哪条守卫拦下的」需要构造只被该守卫拦下的输入 —— 而尺寸大到
// 让 QImage 构造失败（≥ 约 1 GB）时，进程本身就不该被测试拖着去分配，
// 所以 isNull() 那条分支在正常内存下**无法自动化覆盖**。
void TestScreenFrame::rejectsAbsurdDimensionsBeforeAllocating()
{
    QByteArray payload;
    const auto putU32 = [&payload](quint32 v) {
        payload.append(char(v & 0xFF));
        payload.append(char((v >> 8) & 0xFF));
        payload.append(char((v >> 16) & 0xFF));
        payload.append(char((v >> 24) & 0xFF));
    };
    putU32(100000);   // width 远超 16384
    putU32(100000);   // height 同样离谱
    payload.append(64, '\0');

    QVERIFY(lanrdp::decodeRawFrame(payload).isNull());
}

// 尺寸合法但载荷字节数不够装下声明像素时也必须拒 —— 这条**能**定向区分长度守卫：
// 若删掉 `payload.size() - kRawFrameHeaderSize < pixelBytes` 这一检查，就会按
// 64x64 去读只有 1000 字节的载荷（越界读），返回值不再是空图。
void TestScreenFrame::rejectsPayloadTooSmallForDeclaredPixels()
{
    QByteArray payload;
    const auto putU32 = [&payload](quint32 v) {
        payload.append(char(v & 0xFF));
        payload.append(char((v >> 8) & 0xFF));
        payload.append(char((v >> 16) & 0xFF));
        payload.append(char((v >> 24) & 0xFF));
    };
    putU32(64);
    putU32(64);            // 声明 64x64 = 16384 像素 = 65536 字节
    payload.append(1000, '\0');   // 只给 1000 字节

    QVERIFY(lanrdp::decodeRawFrame(payload).isNull());
}

QTEST_GUILESS_MAIN(TestScreenFrame)
#include "test_screenframe.moc"
