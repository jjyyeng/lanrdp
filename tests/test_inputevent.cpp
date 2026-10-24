#include <QtTest/QtTest>

#include "protocol/InputEvent.h"

// INPUT_EVENT 载荷编解码
class TestInputEvent : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsMouseMove();
    void roundTripsMouseButton();
    void roundTripsMouseWheel();
    void roundTripsKeyWithFlags();
    void usesFixedPayloadSizes();
    void rejectsTruncatedPayload();
    void rejectsUnknownKind();

    // 鼠标按键 / 滚轮必须自带坐标 —— 否则被控端会把它们打在**远端的旧光标处**
    //（Windows 的 SendInput 对按键 / 滚轮默认沿用当前光标位置）。
    void mouseButtonCarriesPosition();
    void mouseWheelCarriesPosition();
    // 长度护栏：3 字节的旧布局必须被拒（严格校验「不多不少」）
    void rejectsLegacyShortMousePayload();
};

void TestInputEvent::roundTripsMouseMove()
{
    lanrdp::InputEvent event;
    event.kind = lanrdp::InputKind::MouseMove;
    event.nx   = 32768;
    event.ny   = 65535;

    const QByteArray payload = lanrdp::encodeInputEvent(event);
    QCOMPARE(payload.size(), 5);

    const auto decoded = lanrdp::decodeInputEvent(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(int(decoded->kind), int(lanrdp::InputKind::MouseMove));
    QCOMPARE(decoded->nx, quint16(32768));
    QCOMPARE(decoded->ny, quint16(65535));
}

void TestInputEvent::roundTripsMouseButton()
{
    lanrdp::InputEvent event;
    event.kind    = lanrdp::InputKind::MouseButton;
    event.button  = lanrdp::MouseButtonId::Right;
    event.pressed = true;

    const QByteArray payload = lanrdp::encodeInputEvent(event);
    QCOMPARE(payload.size(), 7);

    const auto decoded = lanrdp::decodeInputEvent(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(int(decoded->kind), int(lanrdp::InputKind::MouseButton));
    QCOMPARE(int(decoded->button), int(lanrdp::MouseButtonId::Right));
    QCOMPARE(decoded->pressed, true);

    event.pressed = false;
    const auto released = lanrdp::decodeInputEvent(lanrdp::encodeInputEvent(event));
    QVERIFY(released.has_value());
    QCOMPARE(released->pressed, false);
}

void TestInputEvent::roundTripsMouseWheel()
{
    for (const int delta : {120, -120}) {
        lanrdp::InputEvent event;
        event.kind       = lanrdp::InputKind::MouseWheel;
        event.wheelDelta = static_cast<std::int16_t>(delta);

        const auto decoded = lanrdp::decodeInputEvent(lanrdp::encodeInputEvent(event));
        QVERIFY(decoded.has_value());
        QCOMPARE(int(decoded->kind), int(lanrdp::InputKind::MouseWheel));
        QCOMPARE(int(decoded->wheelDelta), delta);
    }
}

void TestInputEvent::roundTripsKeyWithFlags()
{
    lanrdp::InputEvent event;
    event.kind  = lanrdp::InputKind::KeyDown;
    event.vk    = 0x25;  // VK_LEFT
    event.flags = lanrdp::InputFlagExtended;

    const QByteArray payload = lanrdp::encodeInputEvent(event);
    QCOMPARE(payload.size(), 5);

    const auto decoded = lanrdp::decodeInputEvent(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(int(decoded->kind), int(lanrdp::InputKind::KeyDown));
    QCOMPARE(decoded->vk, quint16(0x25));
    QCOMPARE(decoded->flags, quint16(lanrdp::InputFlagExtended));

    event.kind = lanrdp::InputKind::KeyUp;
    const auto up = lanrdp::decodeInputEvent(lanrdp::encodeInputEvent(event));
    QVERIFY(up.has_value());
    QCOMPARE(int(up->kind), int(lanrdp::InputKind::KeyUp));
}

void TestInputEvent::usesFixedPayloadSizes()
{
    lanrdp::InputEvent event;

    event.kind = lanrdp::InputKind::MouseMove;
    QCOMPARE(lanrdp::encodeInputEvent(event).size(), 5);
    event.kind = lanrdp::InputKind::MouseButton;
    QCOMPARE(lanrdp::encodeInputEvent(event).size(), 7);
    event.kind = lanrdp::InputKind::MouseWheel;
    QCOMPARE(lanrdp::encodeInputEvent(event).size(), 7);
    event.kind = lanrdp::InputKind::KeyDown;
    QCOMPARE(lanrdp::encodeInputEvent(event).size(), 5);
    event.kind = lanrdp::InputKind::KeyUp;
    QCOMPARE(lanrdp::encodeInputEvent(event).size(), 5);
}

void TestInputEvent::rejectsTruncatedPayload()
{
    lanrdp::InputEvent event;
    event.kind = lanrdp::InputKind::MouseMove;

    QByteArray payload = lanrdp::encodeInputEvent(event);
    payload.chop(1);
    QVERIFY(!lanrdp::decodeInputEvent(payload).has_value());

    QVERIFY(!lanrdp::decodeInputEvent(QByteArray()).has_value());

    // 键事件少一个字节也应当被拒
    lanrdp::InputEvent key;
    key.kind = lanrdp::InputKind::KeyDown;
    QByteArray keyPayload = lanrdp::encodeInputEvent(key);
    keyPayload.chop(1);
    QVERIFY(!lanrdp::decodeInputEvent(keyPayload).has_value());
}

void TestInputEvent::rejectsUnknownKind()
{
    QByteArray payload(5, '\0');
    payload[0] = static_cast<char>(99);  // 未知 kind
    QVERIFY(!lanrdp::decodeInputEvent(payload).has_value());
}

void TestInputEvent::mouseButtonCarriesPosition()
{
    lanrdp::InputEvent event;
    event.kind    = lanrdp::InputKind::MouseButton;
    event.button  = lanrdp::MouseButtonId::Left;
    event.pressed = true;
    event.nx      = 40000;
    event.ny      = 12345;

    const QByteArray payload = lanrdp::encodeInputEvent(event);
    QCOMPARE(payload.size(), 7);  // kind + button + pressed + nx + ny

    const auto decoded = lanrdp::decodeInputEvent(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(int(decoded->kind), int(lanrdp::InputKind::MouseButton));
    QCOMPARE(int(decoded->button), int(lanrdp::MouseButtonId::Left));
    QCOMPARE(decoded->pressed, true);
    QCOMPARE(decoded->nx, quint16(40000));
    QCOMPARE(decoded->ny, quint16(12345));
}

void TestInputEvent::mouseWheelCarriesPosition()
{
    lanrdp::InputEvent event;
    event.kind       = lanrdp::InputKind::MouseWheel;
    event.wheelDelta = -120;
    event.nx         = 100;
    event.ny         = 65000;

    const QByteArray payload = lanrdp::encodeInputEvent(event);
    QCOMPARE(payload.size(), 7);  // kind + delta + nx + ny

    const auto decoded = lanrdp::decodeInputEvent(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(int(decoded->kind), int(lanrdp::InputKind::MouseWheel));
    QCOMPARE(int(decoded->wheelDelta), -120);
    QCOMPARE(decoded->nx, quint16(100));
    QCOMPARE(decoded->ny, quint16(65000));
}

void TestInputEvent::rejectsLegacyShortMousePayload()
{
    // 旧（v1 输入载荷）布局是 [1][button][pressed] 共 3 字节。
    // 解码端必须严格按「不多不少」校验 —— 否则 3 字节会被当成合法载荷接受，
    // 坐标字段读到越界内存（或静默为 0）⇒ 落点漂到左上角。
    QByteArray legacyButton;
    legacyButton.append(static_cast<char>(lanrdp::InputKind::MouseButton));
    legacyButton.append(static_cast<char>(lanrdp::MouseButtonId::Left));
    legacyButton.append(char(1));
    QVERIFY(!lanrdp::decodeInputEvent(legacyButton).has_value());

    QByteArray legacyWheel;
    legacyWheel.append(static_cast<char>(lanrdp::InputKind::MouseWheel));
    legacyWheel.append(char(120));
    legacyWheel.append(char(0));
    QVERIFY(!lanrdp::decodeInputEvent(legacyWheel).has_value());

    // 多一个字节同样应被拒
    lanrdp::InputEvent wheel;
    wheel.kind = lanrdp::InputKind::MouseWheel;
    QByteArray tooLong = lanrdp::encodeInputEvent(wheel);
    tooLong.append('\0');
    QVERIFY(!lanrdp::decodeInputEvent(tooLong).has_value());
}

QTEST_GUILESS_MAIN(TestInputEvent)
#include "test_inputevent.moc"
