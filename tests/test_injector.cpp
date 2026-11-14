#include <QtTest/QtTest>

#include <QRect>

#include "host/InputInjector.h"
#include "protocol/InputEvent.h"

// 把平台无关的 InputEvent 翻译成 Win32 注入参数（纯函数，不真的动鼠标）
class TestInjector : public QObject
{
    Q_OBJECT

private slots:
    void mapsNormalizedToFullScreen();
    void mapsIntoOffsetMonitor();
    void staysInsideTargetMonitorAtEdges();
    void mapsButtonsToFlags();
    void mapsWheelToDelta();
    void mapsExtendedKeyFlag();

    // 按键 / 滚轮必须**带绝对定位**：不带就落在远端的旧光标处
    //（Windows 的 SendInput 对按键 / 滚轮默认沿用当前光标位置）。
    void mouseButtonCarriesAbsolutePosition();
    void wheelCarriesAbsolutePosition();
    // 释放类事件的判定（供 `force` 语义使用）
    void identifiesReleaseEvents();
    // 逻辑按下掩码：断开 / 收回令牌时「补发松开」的兜底要靠它判断有没有悬空的按下。
    void updatesPressedMaskFromButtonEvents();
    // 逻辑按下的键集合：与鼠标掩码同一用途。⚠️ 键盘比鼠标更要紧 ——
    // 卡住的修饰键会让被控端后续**所有**输入变形（全大写 / 点什么都带组合键）。
    void updatesPressedKeysFromKeyEvents();
    // 左右 Ctrl 共用 0x11：只按 vk 记账会让两颗键互相销账（松开一个、另一个被忽略）。
    void tracksLeftAndRightModifiersSeparately();
    // 补抬起必须带回按下时的扩展位，否则右 Ctrl 被抬成左 Ctrl、永久卡住。
    void keyUpStrokesKeepExtendedBit();
    void releaseKeysIsNoOpOnEmptySet();
};

namespace {

// SendInput 的绝对坐标定义域是 0..65535，线性铺满整个虚拟桌面。
// 这里反向换算回像素，用来断言“注入点确实落在目标显示器内”。
int pixelFromAbs(int abs, int vdOrigin, int vdSize)
{
    if (vdSize <= 1)
        return vdOrigin;
    return vdOrigin
           + static_cast<int>(qRound(static_cast<double>(abs) * (vdSize - 1) / 65535.0));
}

lanrdp::InputEvent moveEvent(quint16 nx, quint16 ny)
{
    lanrdp::InputEvent event;
    event.kind = lanrdp::InputKind::MouseMove;
    event.nx   = nx;
    event.ny   = ny;
    return event;
}

} // namespace

void TestInjector::mapsNormalizedToFullScreen()
{
    const QRect monitor(0, 0, 1920, 1080);
    const QRect desktop = monitor;  // 单显示器：两者重合

    const auto topLeft  = lanrdp::planInjection(moveEvent(0, 0), monitor, desktop);
    const auto bottomRt = lanrdp::planInjection(moveEvent(65535, 65535), monitor, desktop);

    QVERIFY(topLeft.valid);
    QVERIFY(topLeft.isMouse);
    QVERIFY(topLeft.isMove);
    QVERIFY(topLeft.mouseFlags & lanrdp::InjectMouseAbsolute);
    QVERIFY(topLeft.mouseFlags & lanrdp::InjectMouseVirtualDesk);
    QCOMPARE(topLeft.absX, 0);
    QCOMPARE(topLeft.absY, 0);

    QVERIFY(bottomRt.valid);
    QCOMPARE(bottomRt.absX, 65535);
    QCOMPARE(bottomRt.absY, 65535);
}

void TestInjector::mapsIntoOffsetMonitor()
{
    // 双显示器：目标是被抓的右屏（x 从 1920 起）
    const QRect desktop(0, 0, 3840, 1080);
    const QRect monitor(1920, 0, 1920, 1080);

    const auto plan = lanrdp::planInjection(moveEvent(0, 0), monitor, desktop);
    QVERIFY(plan.valid);

    const int px = pixelFromAbs(plan.absX, desktop.left(), desktop.width());
    QCOMPARE(px, 1920);  // 应当落在右屏左边缘，而不是整个桌面的左边缘
    QVERIFY(monitor.contains(QPoint(px, plan.absY)));
}

void TestInjector::staysInsideTargetMonitorAtEdges()
{
    const QRect desktop(0, 0, 3840, 1080);
    const QRect monitor(1920, 0, 1920, 1080);

    for (const quint16 nx : {quint16(0), quint16(1), quint16(32767), quint16(32768), quint16(65534), quint16(65535)}) {
        const auto plan = lanrdp::planInjection(moveEvent(nx, 0), monitor, desktop);
        QVERIFY(plan.valid);
        QVERIFY(plan.absX >= 0);
        QVERIFY(plan.absX <= 65535);

        const int px = pixelFromAbs(plan.absX, desktop.left(), desktop.width());
        QVERIFY2(monitor.contains(QPoint(px, plan.absY)),
                 qPrintable(QStringLiteral("nx=%1 -> px=%2 落在目标显示器之外").arg(nx).arg(px)));
    }
}

void TestInjector::mapsButtonsToFlags()
{
    lanrdp::InputEvent event;
    event.kind    = lanrdp::InputKind::MouseButton;
    event.button  = lanrdp::MouseButtonId::Left;
    event.pressed = true;

    const QRect rect(0, 0, 800, 600);

    auto plan = lanrdp::planInjection(event, rect, rect);
    QVERIFY(plan.valid);
    QVERIFY(plan.isButton);
    QVERIFY(plan.mouseFlags & lanrdp::InjectMouseLeftDown);
    QVERIFY(!(plan.mouseFlags & lanrdp::InjectMouseLeftUp));

    event.pressed = false;
    plan          = lanrdp::planInjection(event, rect, rect);
    QVERIFY(plan.mouseFlags & lanrdp::InjectMouseLeftUp);
    QVERIFY(!(plan.mouseFlags & lanrdp::InjectMouseLeftDown));

    event.button  = lanrdp::MouseButtonId::Right;
    event.pressed = true;
    plan          = lanrdp::planInjection(event, rect, rect);
    QVERIFY(plan.mouseFlags & lanrdp::InjectMouseRightDown);

    event.button = lanrdp::MouseButtonId::Middle;
    plan         = lanrdp::planInjection(event, rect, rect);
    QVERIFY(plan.mouseFlags & lanrdp::InjectMouseMiddleDown);
}

void TestInjector::mapsWheelToDelta()
{
    lanrdp::InputEvent event;
    event.kind       = lanrdp::InputKind::MouseWheel;
    event.wheelDelta = 120;

    const QRect rect(0, 0, 800, 600);

    auto plan = lanrdp::planInjection(event, rect, rect);
    QVERIFY(plan.valid);
    QVERIFY(plan.isWheel);
    QVERIFY(plan.mouseFlags & lanrdp::InjectMouseWheel);
    QCOMPARE(plan.wheelDelta, 120);

    event.wheelDelta = -240;
    plan             = lanrdp::planInjection(event, rect, rect);
    QCOMPARE(plan.wheelDelta, -240);
}

void TestInjector::mapsExtendedKeyFlag()
{
    lanrdp::InputEvent event;
    event.kind  = lanrdp::InputKind::KeyDown;
    event.vk    = 0x25;  // VK_LEFT，属于扩展键
    event.flags = lanrdp::InputFlagExtended;

    const QRect rect(0, 0, 800, 600);

    auto plan = lanrdp::planInjection(event, rect, rect);
    QVERIFY(plan.valid);
    QVERIFY(plan.isKey);
    QVERIFY(!plan.keyUp);
    QCOMPARE(plan.vk, quint16(0x25));
    QVERIFY(plan.keyFlags & lanrdp::InjectKeyExtended);
    QVERIFY(!(plan.keyFlags & lanrdp::InjectKeyUp));

    event.kind  = lanrdp::InputKind::KeyUp;
    event.flags = lanrdp::InputFlagNone;
    plan        = lanrdp::planInjection(event, rect, rect);
    QVERIFY(plan.keyUp);
    QVERIFY(plan.keyFlags & lanrdp::InjectKeyUp);
    QVERIFY(!(plan.keyFlags & lanrdp::InjectKeyExtended));
}

void TestInjector::mouseButtonCarriesAbsolutePosition()
{
    // 1920x1080 单屏。nx/ny = 正中 ⇒ 期望落在像素 (960, 540) 附近。
    const QRect rect(0, 0, 1920, 1080);

    lanrdp::InputEvent event;
    event.kind    = lanrdp::InputKind::MouseButton;
    event.button  = lanrdp::MouseButtonId::Left;
    event.pressed = true;
    event.nx      = 32768;
    event.ny      = 32768;

    const auto plan = lanrdp::planInjection(event, rect, rect);
    QVERIFY(plan.valid);
    QVERIFY(plan.isButton);
    QVERIFY2(plan.mouseFlags & lanrdp::InjectMouseAbsolute,
             "按键必须带绝对定位，否则会打在远端的旧光标处");

    const int px = pixelFromAbs(plan.absX, rect.left(), rect.width());
    const int py = pixelFromAbs(plan.absY, rect.top(), rect.height());
    QVERIFY2(qAbs(px - 960) <= 1, qPrintable(QStringLiteral("absX 换算回像素 = %1，应在 960 附近").arg(px)));
    QVERIFY2(qAbs(py - 540) <= 1, qPrintable(QStringLiteral("absY 换算回像素 = %1，应在 540 附近").arg(py)));

    // 坐标 0/0 也要真的落在左上角，而不是「沿用旧位置」
    event.nx = 0;
    event.ny = 0;
    const auto corner = lanrdp::planInjection(event, rect, rect);
    QVERIFY(corner.mouseFlags & lanrdp::InjectMouseAbsolute);
    QCOMPARE(corner.absX, 0);
    QCOMPARE(corner.absY, 0);
}

void TestInjector::wheelCarriesAbsolutePosition()
{
    // 滚轮同样必须带坐标 —— Windows 的滚轮打在「当前光标处」，
    // 不带坐标就会滚错窗口。
    const QRect monitor(1920, 0, 1920, 1080);  // 双屏的右屏
    const QRect desktop(0, 0, 3840, 1080);

    lanrdp::InputEvent event;
    event.kind       = lanrdp::InputKind::MouseWheel;
    event.wheelDelta = 120;
    event.nx         = 0;  // 右屏最左
    event.ny         = 32768;

    const auto plan = lanrdp::planInjection(event, monitor, desktop);
    QVERIFY(plan.valid);
    QVERIFY(plan.isWheel);
    QVERIFY(plan.mouseFlags & lanrdp::InjectMouseWheel);
    QCOMPARE(plan.wheelDelta, 120);
    QVERIFY2(plan.mouseFlags & lanrdp::InjectMouseAbsolute,
             "滚轮必须带绝对定位，否则会滚到旧光标所在的窗口");

    const int px = pixelFromAbs(plan.absX, desktop.left(), desktop.width());
    const int py = pixelFromAbs(plan.absY, desktop.top(), desktop.height());
    QVERIFY2(monitor.contains(QPoint(px, py)),
             qPrintable(QStringLiteral("滚轮落点 (%1,%2) 落在目标显示器之外").arg(px).arg(py)));
}

void TestInjector::identifiesReleaseEvents()
{
    // 「释放类事件」= KeyUp，或 MouseButton 且 pressed == false。
    // 这条判定是 `ClientConnection::sendInput(force=true)` 的唯一依据。
    lanrdp::InputEvent keyUp;
    keyUp.kind = lanrdp::InputKind::KeyUp;
    QVERIFY(lanrdp::isReleaseEvent(keyUp));

    lanrdp::InputEvent keyDown;
    keyDown.kind = lanrdp::InputKind::KeyDown;
    QVERIFY(!lanrdp::isReleaseEvent(keyDown));

    lanrdp::InputEvent buttonUp;
    buttonUp.kind    = lanrdp::InputKind::MouseButton;
    buttonUp.pressed = false;
    QVERIFY(lanrdp::isReleaseEvent(buttonUp));

    lanrdp::InputEvent buttonDown;
    buttonDown.kind    = lanrdp::InputKind::MouseButton;
    buttonDown.pressed = true;
    QVERIFY(!lanrdp::isReleaseEvent(buttonDown));

    lanrdp::InputEvent move;
    move.kind = lanrdp::InputKind::MouseMove;
    QVERIFY(!lanrdp::isReleaseEvent(move));

    lanrdp::InputEvent wheel;
    wheel.kind = lanrdp::InputKind::MouseWheel;
    QVERIFY(!lanrdp::isReleaseEvent(wheel));
}

// 逻辑按下掩码：位 0 左 / 位 1 右 / 位 2 中。
// 它只在「注入成功」后按事件更新，并在断开 / 收回令牌 / 停止监听时决定要不要补一针松开。
// 所以这里要钉住三件事：叠加正确、重复按下幂等、非按键事件一律不动。
void TestInjector::updatesPressedMaskFromButtonEvents()
{
    using lanrdp::InputEvent;
    using lanrdp::InputKind;
    using lanrdp::MouseButtonId;
    using lanrdp::updatePressedMask;

    InputEvent press;
    press.kind    = InputKind::MouseButton;
    press.button  = MouseButtonId::Left;
    press.pressed = true;

    quint32 m = updatePressedMask(0, press);
    QCOMPARE(m, quint32(0x1));

    // 同一个键重复「按下」：必须幂等 —— 否则掩码会被写成 0x3 这种不存在的组合
    m = updatePressedMask(m, press);
    QCOMPARE(m, quint32(0x1));

    InputEvent rpress = press;
    rpress.button = MouseButtonId::Right;
    m = updatePressedMask(m, rpress);
    QCOMPARE(m, quint32(0x3));

    InputEvent mpress = press;
    mpress.button = MouseButtonId::Middle;
    m = updatePressedMask(m, mpress);
    QCOMPARE(m, quint32(0x7));

    // 只松开左键，其余两键保持按下
    InputEvent lrelease = press;
    lrelease.pressed = false;
    m = updatePressedMask(m, lrelease);
    QCOMPARE(m, quint32(0x6));

    // 非鼠标按键事件一律不动掩码（移动 / 滚轮 / 键盘都不该改变「谁还按着」）
    InputEvent move;
    move.kind = InputKind::MouseMove;
    QCOMPARE(updatePressedMask(m, move), quint32(0x6));

    InputEvent wheel;
    wheel.kind = InputKind::MouseWheel;
    QCOMPARE(updatePressedMask(m, wheel), quint32(0x6));

    InputEvent key;
    key.kind = InputKind::KeyDown;
    QCOMPARE(updatePressedMask(m, key), quint32(0x6));

    // 全部松开 ⇒ 归零（归零后断开时就不会多此一举去补针）
    InputEvent rrelease = press;
    rrelease.button  = MouseButtonId::Right;
    rrelease.pressed = false;
    InputEvent mrelease = press;
    mrelease.button  = MouseButtonId::Middle;
    mrelease.pressed = false;

    m = updatePressedMask(m, rrelease);
    m = updatePressedMask(m, mrelease);
    QCOMPARE(m, quint32(0));
}

// 逻辑按下的键集合。三件事要钉住：按下入集、抬起出集、非键盘事件一律不动。
// 它与鼠标掩码一起决定「断开时要不要替对端补一针抬起」——
// 所以「没按下过的键」绝不能凭空出现在集合里（那会向系统灌一针无中生有的抬起）。
void TestInjector::updatesPressedKeysFromKeyEvents()
{
    using lanrdp::InputEvent;
    using lanrdp::InputKind;
    using lanrdp::updatePressedKeys;

    InputEvent shiftDown;
    shiftDown.kind = InputKind::KeyDown;
    shiftDown.vk   = 0x10;   // VK_SHIFT

    QSet<quint32> keys = updatePressedKeys({}, shiftDown);
    QVERIFY(keys.contains(quint32(0x10)));
    QCOMPARE(keys.size(), 1);

    // 长按会连发 KeyDown：必须幂等
    keys = updatePressedKeys(keys, shiftDown);
    QCOMPARE(keys.size(), 1);

    InputEvent ctrlDown = shiftDown;
    ctrlDown.vk         = 0x11;   // VK_CONTROL
    keys                = updatePressedKeys(keys, ctrlDown);
    QCOMPARE(keys.size(), 2);

    // 只松开 Shift，Ctrl 仍在按下
    InputEvent shiftUp = shiftDown;
    shiftUp.kind       = InputKind::KeyUp;
    keys               = updatePressedKeys(keys, shiftUp);
    QCOMPARE(keys.size(), 1);
    QVERIFY(keys.contains(quint32(0x11)));

    // 鼠标三类事件都不该动键盘集合
    InputEvent move;
    move.kind = InputKind::MouseMove;
    QCOMPARE(updatePressedKeys(keys, move).size(), 1);

    InputEvent wheel;
    wheel.kind = InputKind::MouseWheel;
    QCOMPARE(updatePressedKeys(keys, wheel).size(), 1);

    InputEvent button;
    button.kind    = InputKind::MouseButton;
    button.pressed = true;
    QCOMPARE(updatePressedKeys(keys, button).size(), 1);

    // 抬起一个从没按下过的键（对端重连后的残留抬起）：集合不该新增条目
    InputEvent strayUp;
    strayUp.kind = InputKind::KeyUp;
    strayUp.vk   = 0x45;   // 'E'
    QCOMPARE(updatePressedKeys(keys, strayUp).size(), 1);
}

void TestInjector::tracksLeftAndRightModifiersSeparately()
{
    using lanrdp::InputEvent;
    using lanrdp::InputFlagExtended;
    using lanrdp::InputKind;
    using lanrdp::updatePressedKeys;

    InputEvent leftCtrl;
    leftCtrl.kind  = InputKind::KeyDown;
    leftCtrl.vk    = 0x11;   // VK_CONTROL
    leftCtrl.flags = lanrdp::InputFlagNone;

    InputEvent rightCtrl = leftCtrl;
    rightCtrl.flags      = InputFlagExtended;   // 右 Ctrl = 同一个 vk + 扩展位

    QSet<quint32> keys = updatePressedKeys({}, leftCtrl);
    keys               = updatePressedKeys(keys, rightCtrl);
    QCOMPARE(keys.size(), 2);   // 两颗不同的键，不能合并成一条

    // 松开左 Ctrl：右 Ctrl 必须仍在按下（只按 vk 记账时这条会失败）
    InputEvent leftUp = leftCtrl;
    leftUp.kind       = InputKind::KeyUp;
    keys              = updatePressedKeys(keys, leftUp);
    QCOMPARE(keys.size(), 1);
    QVERIFY(keys.contains(lanrdp::pressedKeyToken(0x11, true)));

    // 松开右 Ctrl 才清空
    InputEvent rightUp = rightCtrl;
    rightUp.kind       = InputKind::KeyUp;
    keys               = updatePressedKeys(keys, rightUp);
    QVERIFY(keys.isEmpty());
}

void TestInjector::keyUpStrokesKeepExtendedBit()
{
    // 这是「补发出厂设置」的那一针：扩展位漏了，右 Ctrl 会被抬成左 Ctrl，
    // 被控端那个键永久停在按下态（实测 GetAsyncKeyState(VK_RCONTROL) 一直为按下）。
    const QSet<quint32> keys = { lanrdp::pressedKeyToken(0x11, false),
                                 lanrdp::pressedKeyToken(0x11, true),
                                 lanrdp::pressedKeyToken(0xA1, false) };   // VK_RSHIFT

    const QVector<lanrdp::KeyUpStroke> strokes = lanrdp::keyUpStrokes(keys);
    QCOMPARE(strokes.size(), 3);

    int plainCtrl = 0;
    int extendedCtrl = 0;
    int rightShift = 0;
    for (const lanrdp::KeyUpStroke &stroke : strokes) {
        if (stroke.vk == 0x11 && !stroke.extended)
            ++plainCtrl;
        else if (stroke.vk == 0x11 && stroke.extended)
            ++extendedCtrl;
        else if (stroke.vk == 0xA1 && !stroke.extended)
            ++rightShift;
    }
    QCOMPARE(plainCtrl, 1);
    QCOMPARE(extendedCtrl, 1);   // 左、右 Ctrl 各一针，且只有右边带扩展位
    QCOMPARE(rightShift, 1);     // 右 Shift 走自己的 VK，不带扩展位
}

void TestInjector::releaseKeysIsNoOpOnEmptySet()
{
    // 空集合必须直接返回成功、不发针 —— 正常操作（没有卡键）时
    // 这条兜底一次都不该碰系统。
    QVERIFY(lanrdp::releaseKeys({}));
}

QTEST_GUILESS_MAIN(TestInjector)
#include "test_injector.moc"
