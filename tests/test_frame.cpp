#include <QtTest/QtTest>

#include "protocol/Frame.h"

// Frame 协议结构（编解码往返）
// 帧线格式：Magic[2]='LR' | Version[1] | Type[1] | PayloadLen[4 LE] | Payload[N]
class TestFrame : public QObject
{
    Q_OBJECT

private slots:
    void roundTripHandshakeReq();
    void rejectsBadMagic();
    void rejectsTruncatedHeader();
    // 帧长必须设上限，荒唐声明不得进入「无限等待」
    void rejectsAbsurdPayloadLen();
    void rejectsPayloadLenAtUpperBoundPlusOne();
    void acceptsPayloadLenAtLimit();
    void headerOnlyAbsurdLenIsInvalidNotIncomplete();
    // 发送侧对称自检：超限必须拒发，不能悄悄截断长度字段
    void serializeRefusesOversizePayload();
    // offset 解析（Session 偏移分帧的基础）
    void parsesFromOffsetWithoutCopyingPrefix();
    void offsetIntoMiddleOfFrameIsInvalidNotCrash();
    void offsetBeyondEndIsInvalid();
};

void TestFrame::roundTripHandshakeReq()
{
    lanrdp::Frame f;
    f.type    = 0x01; // HANDSHAKE_REQ（类型枚举延后到 Messages.h 引入）
    f.payload = QByteArrayLiteral("abc");

    const QByteArray wire = f.serialize();

    // 头 8 字节 + payload 3 字节
    QCOMPARE(wire.size(), int(lanrdp::kHeaderSize) + 3);
    QCOMPARE(quint8(wire.at(0)), quint8('L'));
    QCOMPARE(quint8(wire.at(1)), quint8('R'));
    QCOMPARE(quint8(wire.at(2)), quint8(lanrdp::kVersion));
    QCOMPARE(quint8(wire.at(3)), quint8(0x01));
    // PayloadLen = 3（小端）
    QCOMPARE(quint8(wire.at(4)), quint8(3));
    QCOMPARE(quint8(wire.at(5)), quint8(0));
    QCOMPARE(quint8(wire.at(6)), quint8(0));
    QCOMPARE(quint8(wire.at(7)), quint8(0));

    const auto parsed = lanrdp::Frame::parse(wire);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->version, quint8(lanrdp::kVersion));
    QCOMPARE(parsed->type, quint8(0x01));
    QCOMPARE(parsed->payload, QByteArrayLiteral("abc"));
}

void TestFrame::rejectsBadMagic()
{
    QByteArray wire;
    wire.append(char(0x00)); // 错误 magic
    wire.append(char(0x00));
    wire.append(char(lanrdp::kVersion));
    wire.append(char(0x01));
    wire.append(char(0)); wire.append(char(0)); wire.append(char(0)); wire.append(char(0));

    QVERIFY(!lanrdp::Frame::parse(wire).has_value());
}

void TestFrame::rejectsTruncatedHeader()
{
    QByteArray wire(4, '\0'); // 不足 8 字节头
    QVERIFY(!lanrdp::Frame::parse(wire).has_value());
}

// ---- 帧长上限 ----

// 造一个「头合法 + 声明 len 荒唐 + 无载荷」的字节流。
static QByteArray makeHeader(std::uint32_t payloadLen)
{
    QByteArray wire;
    wire.append(char('L'));
    wire.append(char('R'));
    wire.append(char(lanrdp::kVersion));
    wire.append(char(0x05)); // 类型随便给个合法的
    // PayloadLen 小端 4 字节
    wire.append(char(payloadLen & 0xFF));
    wire.append(char((payloadLen >> 8) & 0xFF));
    wire.append(char((payloadLen >> 16) & 0xFF));
    wire.append(char((payloadLen >> 24) & 0xFF));
    return wire;
}

// 0xFFFFFFFF / 0x7FFFFFFF 这类声明绝不能落进「等更多字节」，必须判 Invalid。
void TestFrame::rejectsAbsurdPayloadLen()
{
    for (std::uint32_t len : {0xFFFFFFFFu, 0x7FFFFFFFu}) {
        const QByteArray wire = makeHeader(len);
        lanrdp::Frame     out;
        qsizetype         consumed = -1;
        QCOMPARE(lanrdp::Frame::tryParse(wire, &out, &consumed),
                 lanrdp::ParseStatus::Invalid);
        QVERIFY(!lanrdp::Frame::parse(wire).has_value());
    }
}

// 上限 +1 必须被拒；上限本身发满载荷必须通过（防「一刀切拒绝」的过度修复）。
void TestFrame::rejectsPayloadLenAtUpperBoundPlusOne()
{
    const QByteArray wire = makeHeader(lanrdp::kMaxPayloadLen + 1u);
    QCOMPARE(lanrdp::Frame::tryParse(wire, nullptr, nullptr),
             lanrdp::ParseStatus::Invalid);
}

void TestFrame::acceptsPayloadLenAtLimit()
{
    // 恰好等于上限：声明合法，但载荷未到齐 -> 应为 Incomplete（而非 Invalid）。
    const QByteArray wire = makeHeader(lanrdp::kMaxPayloadLen);
    QCOMPARE(lanrdp::Frame::tryParse(wire, nullptr, nullptr),
             lanrdp::ParseStatus::Incomplete);
}

// 仅 8 字节头 + 荒唐 len：必须 Invalid（断开），而不是 Incomplete（永等）。
void TestFrame::headerOnlyAbsurdLenIsInvalidNotIncomplete()
{
    const QByteArray wire = makeHeader(0xFFFFFFF0u);
    QVERIFY(lanrdp::Frame::tryParse(wire, nullptr, nullptr)
            != lanrdp::ParseStatus::Incomplete);
    QCOMPARE(lanrdp::Frame::tryParse(wire, nullptr, nullptr),
             lanrdp::ParseStatus::Invalid);
}

// Session 用 offset 在缓冲里推进下标（不拷贝前缀）。
// 前置垃圾 + 合法帧：从垃圾长度处开始必须能解析出该帧。
void TestFrame::parsesFromOffsetWithoutCopyingPrefix()
{
    QByteArray buf;
    buf.append(QByteArrayLiteral("junkjunk"));  // 已被消费的前缀（故意不是合法帧头）

    lanrdp::Frame f;
    f.type    = 0x07;
    f.payload = QByteArrayLiteral("payload-after-offset");
    buf.append(f.serialize());

    const qsizetype offset = qsizetype(strlen("junkjunk"));

    lanrdp::Frame out;
    qsizetype     consumed = -1;
    QCOMPARE(lanrdp::Frame::tryParse(buf, &out, &consumed, offset),
             lanrdp::ParseStatus::Ok);
    QCOMPARE(out.type, quint8(0x07));
    QCOMPARE(out.payload, QByteArrayLiteral("payload-after-offset"));
    // consumed 只报该帧自身长度，不含 offset
    QCOMPARE(consumed, f.serialize().size());
}

// offset 落在帧中间：必须判 Invalid（不能越界读、不能崩）。
void TestFrame::offsetIntoMiddleOfFrameIsInvalidNotCrash()
{
    lanrdp::Frame f;
    f.type    = 0x05;
    f.payload = QByteArrayLiteral("abc");
    const QByteArray wire = f.serialize();

    lanrdp::Frame out;
    // offset = 3 落在帧头中间，读到的是 payload 字节而不是 magic
    const lanrdp::ParseStatus st = lanrdp::Frame::tryParse(wire, &out, nullptr, 3);
    QCOMPARE(st, lanrdp::ParseStatus::Invalid);
}

void TestFrame::offsetBeyondEndIsInvalid()
{
    lanrdp::Frame f;
    f.type = 0x02;
    const QByteArray wire = f.serialize();

    QCOMPARE(lanrdp::Frame::tryParse(wire, nullptr, nullptr, wire.size() + 10),
             lanrdp::ParseStatus::Invalid);
    QCOMPARE(lanrdp::Frame::tryParse(wire, nullptr, nullptr, -1),
             lanrdp::ParseStatus::Invalid);
}

// 载荷超过协议上限时必须**拒发**，而不是把 32 位长度字段截断成另一个值 ——
// 被截断的帧到对端会「按声明长度切一刀」，多出来的字节被当成下一帧的魔数，
// 整条流立刻判损坏、连接断掉，现场还无从诊断。
// 裸帧是最容易撞线的一类（4K BGRA 已经用到上限的 99%）。
void TestFrame::serializeRefusesOversizePayload()
{
    lanrdp::Frame f;
    f.type = 0x02;

    f.payload = QByteArray(int(lanrdp::kMaxPayloadLen) + 1, '\0');
    QVERIFY2(f.serialize().isEmpty(), "超过上限的帧必须发不出去");

    // 正好等于上限是合法的，不能连它也一起拒
    f.payload = QByteArray(int(lanrdp::kMaxPayloadLen), '\0');
    QCOMPARE(f.serialize().size(),
             int(lanrdp::kHeaderSize) + int(lanrdp::kMaxPayloadLen));
}

QTEST_APPLESS_MAIN(TestFrame)
#include "test_frame.moc"
