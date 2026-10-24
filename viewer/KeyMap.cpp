#include "viewer/KeyMap.h"

#include <QtCore/Qt>

namespace lanrdp {

std::uint16_t qtKeyToVirtualKey(int qtKey, Qt::KeyboardModifiers mods)
{
    // 「这是小键盘吗」必须先判 —— 小键盘的 - . / * 与主键盘同名同 key()，
    // 只有 Modifier 能区分，而它们对应的 VK 完全不同（VK_SUBTRACT vs VK_OEM_MINUS…）。
    const bool keypad = mods.testFlag(Qt::KeypadModifier);
    if (keypad) {
        if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)
            return static_cast<std::uint16_t>(0x60 + (qtKey - Qt::Key_0));   // VK_NUMPAD0..9
        switch (qtKey) {
        case Qt::Key_Asterisk:
        case Qt::Key_multiply:  return 0x6A;   // VK_MULTIPLY
        case Qt::Key_Plus:      return 0x6B;   // VK_ADD
        case Qt::Key_Minus:     return 0x6D;   // VK_SUBTRACT
        case Qt::Key_Period:
        case Qt::Key_Comma:     return 0x6E;   // VK_DECIMAL（小数点键，随区域可能是逗号）
        case Qt::Key_Slash:     return 0x6F;   // VK_DIVIDE
        default:                break;
        }
    }

    // 先处理需要显式对应的控制键与符号键，
    // 再统一处理「Qt 码与 VK 码同值」的字母 / 数字 / 功能键区间，
    // 这样区间判断绝不会误伤符号键（例如 Qt::Key_Semicolon 与数字区间相邻）。
    switch (qtKey) {
    case Qt::Key_Escape:       return 0x1B;
    case Qt::Key_Tab:          return 0x09;
    case Qt::Key_Backtab:      return 0x09;
    case Qt::Key_Backspace:    return 0x08;
    case Qt::Key_Return:       return 0x0D;
    case Qt::Key_Enter:        return 0x0D;
    case Qt::Key_Insert:       return 0x2D;
    case Qt::Key_Delete:       return 0x2E;
    case Qt::Key_Pause:        return 0x13;
    case Qt::Key_Print:        return 0x2C;
    case Qt::Key_Home:         return 0x24;
    case Qt::Key_End:          return 0x23;
    case Qt::Key_Left:         return 0x25;
    case Qt::Key_Up:           return 0x26;
    case Qt::Key_Right:        return 0x27;
    case Qt::Key_Down:         return 0x28;
    case Qt::Key_PageUp:       return 0x21;
    case Qt::Key_PageDown:     return 0x22;
    case Qt::Key_Shift:        return 0x10;
    case Qt::Key_Control:      return 0x11;
    case Qt::Key_Alt:          return 0x12;
    case Qt::Key_Meta:         return 0x5B;  // VK_LWIN
    case Qt::Key_CapsLock:     return 0x14;
    case Qt::Key_NumLock:      return 0x90;
    case Qt::Key_ScrollLock:   return 0x91;
    case Qt::Key_Space:        return 0x20;
    case Qt::Key_Minus:        return 0xBD;
    case Qt::Key_Equal:        return 0xBB;
    case Qt::Key_BracketLeft:  return 0xDB;
    case Qt::Key_BracketRight: return 0xDD;
    case Qt::Key_Backslash:    return 0xDC;
    case Qt::Key_Semicolon:    return 0xBA;
    case Qt::Key_Apostrophe:   return 0xDE;
    case Qt::Key_QuoteLeft:    return 0xC0;
    case Qt::Key_Comma:        return 0xBC;
    case Qt::Key_Period:       return 0xBE;
    case Qt::Key_Slash:        return 0xBF;
    default:                   break;
    }

    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)
        return static_cast<std::uint16_t>(qtKey);  // 0x41..0x5A

    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)
        return static_cast<std::uint16_t>(qtKey);  // 0x30..0x39

    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12)
        return static_cast<std::uint16_t>(0x70 + (qtKey - Qt::Key_F1));  // VK_F1..VK_F12

    return 0;  // 暂不支持
}

bool qtKeyIsExtended(int qtKey, Qt::KeyboardModifiers mods)
{
    // 小键盘除号是 VK_DIVIDE —— 按扩展键规范必须置位，否则被控端收到的是
    // 主键盘斜杠（VK_OEM_2），行为完全不同。
    if (mods.testFlag(Qt::KeypadModifier) && qtKey == Qt::Key_Slash)
        return true;

    switch (qtKey) {
    case Qt::Key_Insert:
    case Qt::Key_Delete:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Left:
    case Qt::Key_Up:
    case Qt::Key_Right:
    case Qt::Key_Down:
    case Qt::Key_Print:
        return true;
    default:
        return false;
    }
}

} // namespace lanrdp
