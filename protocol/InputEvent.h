#pragma once

#include <QByteArray>

#include <cstdint>
#include <optional>

namespace lanrdp {

// INPUT_EVENT（type=5）的子类型
enum class InputKind : std::uint8_t
{
    MouseMove   = 0,
    MouseButton = 1,
    MouseWheel  = 2,
    KeyDown     = 3,
    KeyUp       = 4,
};

enum class MouseButtonId : std::uint8_t
{
    Left   = 1,
    Right  = 2,
    Middle = 3,
};

// 键事件标志位（与 Win32 KEYEVENTF_* 对齐，便于注入端直接使用）
enum InputFlag : std::uint16_t
{
    InputFlagNone     = 0,
    InputFlagExtended = 1 << 0,  // 扩展键：方向键 / 右 Ctrl / 右 Alt / 小键盘等
};

struct InputEvent
{
    InputKind kind = InputKind::MouseMove;

    // MouseMove 专用：在被抓画面内的归一化坐标，0..65535 定点表示 0.0..1.0。
    // 用归一化而非像素，控制端与被控端分辨率不一致时也能正确映射。
    std::uint16_t nx = 0;
    std::uint16_t ny = 0;

    // MouseButton 专用
    MouseButtonId button  = MouseButtonId::Left;
    bool          pressed = false;

    // MouseWheel 专用：每 120 为一格，正数向上（与 WHEEL_DELTA 一致）
    std::int16_t wheelDelta = 0;

    // KeyDown / KeyUp 专用：Windows 虚拟键码 + 标志位
    std::uint16_t vk    = 0;
    std::uint16_t flags = 0;
};

// 载荷布局（首字节固定为 kind，小端）：
// [0][nx u16][ny u16] = 5 鼠标移动
// [1][button u8][pressed u8][nx u16][ny u16] = 7 鼠标按键
// [2][delta i16][nx u16][ny u16] = 7 滚轮
// [3][vk u16][flags u16] = 5 按键按下
// [4][vk u16][flags u16] = 5 按键抬起
//
// ⚠️ 鼠标按键 / 滚轮**必须**带坐标：Windows 的 SendInput 对它们默认打在系统当前
// 光标处，不带就会落在远端的旧位置（「先移鼠标再点」这条最常见的路径反而必错）。
QByteArray encodeInputEvent(const InputEvent &event);

// 载荷非法 / 长度不符 / kind 未知时返回 nullopt。
// ⚠️ 长度校验是「不多不少」：布局一变，这里的常量必须同步改。
std::optional<InputEvent> decodeInputEvent(const QByteArray &payload);

// 是否为「释放类」事件：KeyUp，或鼠标按键且 pressed == false。
// 释放类事件在控制端**永不受门控**（跳过键鼠开关与令牌门控）——
// 一旦被丢弃就永远补不上，远端那个键会永久按下。
inline bool isReleaseEvent(const InputEvent &event)
{
    if (event.kind == InputKind::KeyUp)
        return true;
    return event.kind == InputKind::MouseButton && !event.pressed;
}

} // namespace lanrdp
