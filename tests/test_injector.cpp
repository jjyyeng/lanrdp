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

QTEST_GUILESS_MAIN(TestInjector)
#include "test_injector.moc"
