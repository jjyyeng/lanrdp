#pragma once

#include "protocol/InputEvent.h"

#include <QRect>

#include <cstdint>

namespace lanrdp {

// 平台无关的鼠标注入标志位（与 Win32 MOUSEEVENTF_* 一一对应，映射在 .cpp 内完成，
// 这样头文件不必泄漏 windows.h）。
enum InjectMouseFlag : std::uint32_t
{
    InjectMouseMove        = 1u << 0,
    InjectMouseAbsolute    = 1u << 1,
    InjectMouseVirtualDesk = 1u << 2,
    InjectMouseLeftDown    = 1u << 3,
    InjectMouseLeftUp      = 1u << 4,
    InjectMouseRightDown   = 1u << 5,
    InjectMouseRightUp     = 1u << 6,
    InjectMouseMiddleDown  = 1u << 7,
    InjectMouseMiddleUp    = 1u << 8,
    InjectMouseWheel       = 1u << 9,
};

// 平台无关的键盘注入标志位（对应 Win32 KEYEVENTF_*）
enum InjectKeyFlag : std::uint16_t
{
    InjectKeyUp       = 1u << 0,
    InjectKeyExtended = 1u << 1,
};

// 一次 SendInput 调用的「注入计划」。
// 把 InputEvent 翻译成 Win32 参数是纯函数——可以脱离真实桌面做单元测试，
// 断言坐标与标志位，而不必真的移动本机鼠标。
struct InjectPlan
{
    bool valid   = false;
    bool isMouse = true;
    bool isMove  = false;
    bool isButton = false;
    bool isWheel = false;
    bool isKey   = false;
    bool keyUp   = false;

    std::uint32_t mouseFlags = 0;  // InjectMouseFlag 组合
    std::int32_t  absX       = 0;  // 0..65535，铺满整个虚拟桌面
    std::int32_t  absY       = 0;
    std::int32_t  wheelDelta = 0;
    std::uint16_t vk         = 0;  // Windows 虚拟键码
    std::uint16_t keyFlags   = 0;  // InjectKeyFlag 组合
};

// monitorRect：被抓画面的屏幕像素矩形；virtualDesktop：整个虚拟桌面像素矩形。
//
// 归一化坐标 nx/ny(0..65535) 先还原成 monitorRect 内的像素点，
// 再线性换算成跨整个虚拟桌面的 0..65535 绝对坐标（SendInput 的定义域）。
// 这样即使被控端是多显示器、且抓的不是主屏，注入点也会落在正确的屏上。
InjectPlan planInjection(const InputEvent &event,
                         const QRect       &monitorRect,
                         const QRect       &virtualDesktop);

// 把计划提交给系统。失败返回 false（例如目标窗口权限更高，被 UIPI 拦截）。
bool applyInjection(const InjectPlan &plan);

// 便捷组合：翻译 + 提交。
bool injectEvent(const InputEvent &event, const QRect &monitorRect, const QRect &virtualDesktop);

// 把左/右/中三个鼠标键**各松开一针**（不移动光标）。
//
// 用途：`HostController` 在「断开 / 收回令牌 / 停止监听」时的一次性兜底 ——
// 我们对端那一针「抬起」若没送到（网络正好在按下与抬起之间断掉），被控端的鼠标键
// 会一直停在按下态（表现为拖拽不松手）。这里补一次松开来清掉。
//
// ⚠️ 它**不靠计时猜**：只在「我们确实注入过按下、却还没注入过抬起」时才调用
//（见 `updatePressedMask()` 维护的掩码）。所以不会像定时器那样误伤正常操作。
// ⚠️ 可从任意线程调用：SendInput 无线程亲和性。
bool releaseAllMouseButtons();

// 依据一条**已成功注入**的输入事件更新「逻辑按下掩码」（位 0 左 / 位 1 右 / 位 2 中）。
// 非鼠标按键事件原样返回。抽成纯函数是为了能单测边界，不必真的往系统灌针。
quint32 updatePressedMask(quint32 mask, const InputEvent &event);

// 当前虚拟桌面像素矩形（多显示器时含负坐标）。取不到时退化为主显示器矩形。
QRect virtualDesktopRect();

} // namespace lanrdp
