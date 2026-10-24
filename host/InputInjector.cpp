#include "host/InputInjector.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace lanrdp {

namespace {

// 归一化 0..65535 -> monitorRect 内的像素坐标（钳制在矩形内）。
int pixelFromNormalized(std::uint16_t normalized, int origin, int size)
{
    if (size <= 0)
        return origin;
    qint64 offset = static_cast<qint64>(normalized) * size / 65536;
    if (offset < 0)
        offset = 0;
    if (offset > size - 1)
        offset = size - 1;
    return origin + static_cast<int>(offset);
}

// 像素 -> SendInput 绝对坐标（0..65535 铺满虚拟桌面）。
std::int32_t absoluteFromPixel(int pixel, int vdOrigin, int vdSize)
{
    if (vdSize <= 1)
        return 0;
    const double ratio = static_cast<double>(pixel - vdOrigin) / (vdSize - 1);
    const double scaled = ratio * 65535.0;
    if (scaled <= 0.0)
        return 0;
    if (scaled >= 65535.0)
        return 65535;
    return static_cast<std::int32_t>(qRound(scaled));
}

std::uint32_t toMouseFlags(std::uint32_t neutralFlags)
{
    std::uint32_t flags = 0;
    if (neutralFlags & InjectMouseMove)        flags |= MOUSEEVENTF_MOVE;
    if (neutralFlags & InjectMouseAbsolute)    flags |= MOUSEEVENTF_ABSOLUTE;
    if (neutralFlags & InjectMouseVirtualDesk) flags |= MOUSEEVENTF_VIRTUALDESK;
    if (neutralFlags & InjectMouseLeftDown)    flags |= MOUSEEVENTF_LEFTDOWN;
    if (neutralFlags & InjectMouseLeftUp)      flags |= MOUSEEVENTF_LEFTUP;
    if (neutralFlags & InjectMouseRightDown)   flags |= MOUSEEVENTF_RIGHTDOWN;
    if (neutralFlags & InjectMouseRightUp)     flags |= MOUSEEVENTF_RIGHTUP;
    if (neutralFlags & InjectMouseMiddleDown)  flags |= MOUSEEVENTF_MIDDLEDOWN;
    if (neutralFlags & InjectMouseMiddleUp)    flags |= MOUSEEVENTF_MIDDLEUP;
    if (neutralFlags & InjectMouseWheel)       flags |= MOUSEEVENTF_WHEEL;
    return flags;
}

std::uint32_t toKeyFlags(std::uint16_t neutralFlags)
{
    std::uint32_t flags = 0;
    if (neutralFlags & InjectKeyUp)       flags |= KEYEVENTF_KEYUP;
    if (neutralFlags & InjectKeyExtended) flags |= KEYEVENTF_EXTENDEDKEY;
    return flags;
}

bool rectUsable(const QRect &rect)
{
    return rect.isValid() && rect.width() > 0 && rect.height() > 0;
}

} // namespace

QRect virtualDesktopRect()
{
    const int x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w > 0 && h > 0)
        return QRect(x, y, w, h);

    return QRect(0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN));
}

InjectPlan planInjection(const InputEvent &event, const QRect &monitorRect, const QRect &virtualDesktop)
{
    InjectPlan plan;

    // 监视器矩形不可用时，退化用虚拟桌面顶替，至少保证坐标合法（映射会退化成近似恒等）。
    const QRect monitor = rectUsable(monitorRect) ? monitorRect
                                                  : virtualDesktop;

    const QRect desktop = rectUsable(virtualDesktop) ? virtualDesktop
                                                     : QRect(0, 0, 1920, 1080);

    switch (event.kind) {
    case InputKind::MouseMove: {
        const int px = pixelFromNormalized(event.nx, monitor.left(), monitor.width());
        const int py = pixelFromNormalized(event.ny, monitor.top(), monitor.height());

        plan.valid      = true;
        plan.isMouse    = true;
        plan.isMove     = true;
        plan.mouseFlags = InjectMouseMove | InjectMouseAbsolute | InjectMouseVirtualDesk;
        plan.absX       = absoluteFromPixel(px, desktop.left(), desktop.width());
        plan.absY       = absoluteFromPixel(py, desktop.top(), desktop.height());
        return plan;
    }

    case InputKind::MouseButton: {
        std::uint32_t flags = 0;
        switch (event.button) {
        case MouseButtonId::Left:
            flags = event.pressed ? InjectMouseLeftDown : InjectMouseLeftUp;
            break;
        case MouseButtonId::Right:
            flags = event.pressed ? InjectMouseRightDown : InjectMouseRightUp;
            break;
        case MouseButtonId::Middle:
            flags = event.pressed ? InjectMouseMiddleDown : InjectMouseMiddleUp;
            break;
        }

        // ⚠️ 必须**带绝对定位**：Windows 的按键事件默认打在「系统当前光标处」，
        // 不带就落在远端的旧位置 —— 「先移鼠标再点」这条最常见的路径反而会点错。
        const int px = pixelFromNormalized(event.nx, monitor.left(), monitor.width());
        const int py = pixelFromNormalized(event.ny, monitor.top(), monitor.height());

        plan.valid      = true;
        plan.isMouse    = true;
        plan.isButton   = true;
        plan.mouseFlags = flags
                          | static_cast<std::uint32_t>(InjectMouseAbsolute)
                          | static_cast<std::uint32_t>(InjectMouseVirtualDesk);
        plan.absX       = absoluteFromPixel(px, desktop.left(), desktop.width());
        plan.absY       = absoluteFromPixel(py, desktop.top(), desktop.height());
        return plan;
    }

    case InputKind::MouseWheel: {
        // 滚轮同理：它打在「当前光标处」，不带坐标就会滚错窗口。
        const int px = pixelFromNormalized(event.nx, monitor.left(), monitor.width());
        const int py = pixelFromNormalized(event.ny, monitor.top(), monitor.height());

        plan.valid      = true;
        plan.isMouse    = true;
        plan.isWheel    = true;
        plan.mouseFlags = static_cast<std::uint32_t>(InjectMouseWheel)
                          | static_cast<std::uint32_t>(InjectMouseAbsolute)
                          | static_cast<std::uint32_t>(InjectMouseVirtualDesk);
        plan.absX       = absoluteFromPixel(px, desktop.left(), desktop.width());
        plan.absY       = absoluteFromPixel(py, desktop.top(), desktop.height());
        plan.wheelDelta = event.wheelDelta;
        return plan;
    }

    case InputKind::KeyDown:
    case InputKind::KeyUp: {
        std::uint16_t flags = 0;
        if (event.kind == InputKind::KeyUp)
            flags |= InjectKeyUp;
        if (event.flags & InputFlagExtended)
            flags |= InjectKeyExtended;

        plan.valid    = true;
        plan.isMouse  = false;
        plan.isKey    = true;
        plan.keyUp    = (event.kind == InputKind::KeyUp);
        plan.vk       = event.vk;
        plan.keyFlags = flags;
        return plan;
    }
    }

    return plan;  // valid == false
}

bool applyInjection(const InjectPlan &plan)
{
    if (!plan.valid)
        return false;

    INPUT input{};

    if (plan.isMouse) {
        input.type           = INPUT_MOUSE;
        input.mi.dx          = static_cast<LONG>(plan.absX);
        input.mi.dy          = static_cast<LONG>(plan.absY);
        input.mi.mouseData   = static_cast<DWORD>(plan.wheelDelta);
        input.mi.dwFlags     = toMouseFlags(plan.mouseFlags);
        input.mi.time        = 0;
        input.mi.dwExtraInfo = 0;
    } else {
        input.type           = INPUT_KEYBOARD;
        input.ki.wVk         = static_cast<WORD>(plan.vk);
        input.ki.wScan       = 0;
        input.ki.dwFlags     = toKeyFlags(plan.keyFlags);
        input.ki.time        = 0;
        input.ki.dwExtraInfo = 0;
    }

    return ::SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool injectEvent(const InputEvent &event, const QRect &monitorRect, const QRect &virtualDesktop)
{
    return applyInjection(planInjection(event, monitorRect, virtualDesktop));
}

bool releaseAllMouseButtons()
{
    // 三针一次提交：**不要**带 MOUSEEVENTF_MOVE/ABSOLUTE —— 松开事件只关心键，
    // 带上坐标反而可能被当成一次额外的移动。dwFlags 只有 UP 位即可。
    INPUT inputs[3] = {};
    inputs[0].type       = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    inputs[1].type       = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    inputs[2].type       = INPUT_MOUSE;
    inputs[2].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;

    const UINT sent = ::SendInput(3, inputs, sizeof(INPUT));
    // 三针是原子的：要么全进、要么全被拦（UIPI）。部分成功按失败算。
    return sent == 3;
}

quint32 updatePressedMask(quint32 mask, const InputEvent &event)
{
    if (event.kind != InputKind::MouseButton)
        return mask;

    quint32 bit = 0;
    switch (event.button) {
    case MouseButtonId::Left:   bit = 0x1u; break;
    case MouseButtonId::Right:  bit = 0x2u; break;
    case MouseButtonId::Middle: bit = 0x4u; break;
    default:                    return mask;   // 未知键位：不动掩码，别凭空造出一个「按下」
    }
    return event.pressed ? (mask | bit) : (mask & ~bit);
}

} // namespace lanrdp
