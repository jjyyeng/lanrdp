#include <QtTest/QtTest>

#include <QtCore/Qt>

#include "viewer/KeyMap.h"

// Qt::Key -> Windows 虚拟键码映射
class TestKeyMap : public QObject
{
    Q_OBJECT

private slots:
    void mapsLetters();
    void mapsDigits();
    void mapsWhitespaceAndEditKeys();
    void mapsModifiers();
    void mapsNavigationKeys();
    void marksNavigationKeysExtended();
    void mapsFunctionKeys();
    void mapsPunctuationKeys();
    void mapsKeypadKeysDistinctlyFromMainKeyboard();
    // 左右修饰键：必须靠 nativeScanCode 分（nativeVirtualKey 分不出来）
    void mapsRightSideControlAndAlt();
    void mapsRightSideWinAndShift();
    void treatsAltGrAsRightAlt();
    void withoutScanCodeFallsBackToLeft();
    void returnsZeroForUnsupportedKeys();
};

void TestKeyMap::mapsLetters()
{
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_A), quint16(0x41));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_M), quint16(0x4D));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Z), quint16(0x5A));
}

void TestKeyMap::mapsDigits()
{
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_0), quint16(0x30));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_5), quint16(0x35));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_9), quint16(0x39));
}

void TestKeyMap::mapsWhitespaceAndEditKeys()
{
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Escape), quint16(0x1B));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Tab), quint16(0x09));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Backspace), quint16(0x08));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Return), quint16(0x0D));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Enter), quint16(0x0D));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Space), quint16(0x20));
}

void TestKeyMap::mapsModifiers()
{
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Shift), quint16(0x10));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Control), quint16(0x11));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Alt), quint16(0x12));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_CapsLock), quint16(0x14));
}

void TestKeyMap::mapsNavigationKeys()
{
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Left), quint16(0x25));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Up), quint16(0x26));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Right), quint16(0x27));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Down), quint16(0x28));

    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Home), quint16(0x24));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_End), quint16(0x23));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_PageUp), quint16(0x21));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_PageDown), quint16(0x22));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Insert), quint16(0x2D));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Delete), quint16(0x2E));
}

void TestKeyMap::marksNavigationKeysExtended()
{
    // 不置扩展位的话，被控端会把方向键当成小键盘数字
    for (const int key : {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down,
                          Qt::Key_Home, Qt::Key_End, Qt::Key_Insert, Qt::Key_Delete,
                          Qt::Key_PageUp, Qt::Key_PageDown}) {
        QVERIFY2(lanrdp::qtKeyIsExtended(key),
                 qPrintable(QStringLiteral("Qt::Key(%1) 应标记为扩展键").arg(key)));
    }

    QVERIFY(!lanrdp::qtKeyIsExtended(Qt::Key_A));
    QVERIFY(!lanrdp::qtKeyIsExtended(Qt::Key_Return));
    QVERIFY(!lanrdp::qtKeyIsExtended(Qt::Key_Space));
}

void TestKeyMap::mapsFunctionKeys()
{
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_F1), quint16(0x70));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_F6), quint16(0x75));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_F12), quint16(0x7B));
}

void TestKeyMap::mapsPunctuationKeys()
{
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Minus), quint16(0xBD));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Equal), quint16(0xBB));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Semicolon), quint16(0xBA));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Slash), quint16(0xBF));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Period), quint16(0xBE));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_Comma), quint16(0xBC));
}

// 小键盘必须与主键盘区分开：Qt 把小键盘数字的 `key()` 也报成 `Key_0..9`，
// 只有 `Qt::KeypadModifier` 能说明「这是小键盘」。不区分的话，被控端 NumLock
// 与本地不一致时小键盘会打出完全不同的字符（`-` 变 `_`、数字变方向键）。
void TestKeyMap::mapsKeypadKeysDistinctlyFromMainKeyboard()
{
    using lanrdp::qtKeyIsExtended;
    using lanrdp::qtKeyToVirtualKey;
    const Qt::KeyboardModifiers keypad = Qt::KeypadModifier;

    // 数字：小键盘走 VK_NUMPAD0..9，主键盘仍是 0x30..0x39
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_0, keypad), quint16(0x60));
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_5, keypad), quint16(0x65));
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_9, keypad), quint16(0x69));
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_5), quint16(0x35));           // 主键盘不受影响

    // 运算符：同名同 key()，靠修饰键分出不同的 VK
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_Minus, keypad), quint16(0x6D));     // VK_SUBTRACT
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_Minus), quint16(0xBD));             // VK_OEM_MINUS
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_Plus, keypad), quint16(0x6B));      // VK_ADD
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_Asterisk, keypad), quint16(0x6A));  // VK_MULTIPLY
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_Period, keypad), quint16(0x6E));    // VK_DECIMAL
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_Slash, keypad), quint16(0x6F));     // VK_DIVIDE
    QCOMPARE(qtKeyToVirtualKey(Qt::Key_Period), quint16(0xBE));            // VK_OEM_PERIOD

    // 小键盘除号 = VK_DIVIDE，按规范是扩展键；主键盘斜杠不是。
    QVERIFY(qtKeyIsExtended(Qt::Key_Slash, keypad));
    QVERIFY(!qtKeyIsExtended(Qt::Key_Slash));
    // 小键盘数字**不是**扩展键（标了的话被控端会收到方向键）
    QVERIFY(!qtKeyIsExtended(Qt::Key_5, keypad));
    QVERIFY(!qtKeyIsExtended(Qt::Key_5));
}

void TestKeyMap::mapsRightSideControlAndAlt()
{
    using lanrdp::mapKeyEvent;

    // 左侧：扫描码无 0xE0 前缀
    const lanrdp::KeyMapping lctrl = mapKeyEvent(Qt::Key_Control, Qt::NoModifier, 0x1D);
    QCOMPARE(lctrl.vk, quint16(0x11));      // VK_CONTROL
    QVERIFY(!lctrl.extended);

    // 右侧：Qt 的 nativeScanCode() 会带 0xE0 前缀（实测 0xE01D）
    const lanrdp::KeyMapping rctrl = mapKeyEvent(Qt::Key_Control, Qt::NoModifier, 0xE01D);
    QCOMPARE(rctrl.vk, quint16(0x11));      // 仍是通用码，靠扩展位区分
    QVERIFY(rctrl.extended);

    const lanrdp::KeyMapping lalt = mapKeyEvent(Qt::Key_Alt, Qt::NoModifier, 0x38);
    QCOMPARE(lalt.vk, quint16(0x12));
    QVERIFY(!lalt.extended);

    const lanrdp::KeyMapping ralt = mapKeyEvent(Qt::Key_Alt, Qt::NoModifier, 0xE038);
    QCOMPARE(ralt.vk, quint16(0x12));
    QVERIFY(ralt.extended);
}

void TestKeyMap::mapsRightSideWinAndShift()
{
    using lanrdp::mapKeyEvent;

    // 「VK_LWIN + 扩展位」实测仍是左 Win ⇒ 右 Win 必须换码
    const lanrdp::KeyMapping lwin = mapKeyEvent(Qt::Key_Meta, Qt::NoModifier, 0x5B);
    QCOMPARE(lwin.vk, quint16(0x5B));
    QVERIFY(!lwin.extended);

    const lanrdp::KeyMapping rwin = mapKeyEvent(Qt::Key_Meta, Qt::NoModifier, 0xE05C);
    QCOMPARE(rwin.vk, quint16(0x5C));       // VK_RWIN
    QVERIFY(rwin.extended);

    // 右 Shift 不是扩展键（扫描码 0x36、无 0xE0 前缀），加扩展位也没用，
    // 只能换成 VK_RSHIFT
    const lanrdp::KeyMapping lshift = mapKeyEvent(Qt::Key_Shift, Qt::NoModifier, 0x2A);
    QCOMPARE(lshift.vk, quint16(0x10));     // VK_SHIFT
    QVERIFY(!lshift.extended);

    const lanrdp::KeyMapping rshift = mapKeyEvent(Qt::Key_Shift, Qt::NoModifier, 0x36);
    QCOMPARE(rshift.vk, quint16(0xA1));     // VK_RSHIFT
    QVERIFY(!rshift.extended);
}

void TestKeyMap::treatsAltGrAsRightAlt()
{
    // 部分键盘布局把右 Alt 报成 Qt::Key_AltGr。以前它映射不出来（返回 0）被直接丢弃，
    // 现在按右 Alt 处理。
    const lanrdp::KeyMapping map = lanrdp::mapKeyEvent(Qt::Key_AltGr, Qt::NoModifier, 0xE038);
    QCOMPARE(map.vk, quint16(0x12));
    QVERIFY(map.extended);
}

void TestKeyMap::withoutScanCodeFallsBackToLeft()
{
    // 手搓的 QKeyEvent 没有扫描码（测试、IME、自动化）。这时行为必须与修复前一致：
    // 一律按左侧发，不能凭空冒出扩展位。
    const lanrdp::KeyMapping ctrl = lanrdp::mapKeyEvent(Qt::Key_Control, Qt::NoModifier, 0);
    QCOMPARE(ctrl.vk, quint16(0x11));
    QVERIFY(!ctrl.extended);

    // 非修饰键不受影响：走原来的表
    const lanrdp::KeyMapping del = lanrdp::mapKeyEvent(Qt::Key_Delete, Qt::NoModifier, 0);
    QCOMPARE(del.vk, quint16(0x2E));
    QVERIFY(del.extended);

    const lanrdp::KeyMapping letter = lanrdp::mapKeyEvent(Qt::Key_A, Qt::NoModifier, 0);
    QCOMPARE(letter.vk, quint16(0x41));
    QVERIFY(!letter.extended);
}

void TestKeyMap::returnsZeroForUnsupportedKeys()
{
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_unknown), quint16(0));
    QCOMPARE(lanrdp::qtKeyToVirtualKey(Qt::Key_MediaPlay), quint16(0));
}

QTEST_GUILESS_MAIN(TestKeyMap)
#include "test_keymap.moc"
