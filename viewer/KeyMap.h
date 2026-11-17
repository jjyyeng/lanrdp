#pragma once

#include <QtCore/Qt>

#include <cstdint>

namespace lanrdp {

// Qt::Key -> Windows 虚拟键码（VK_*）。
// 返回 0 表示该键暂不支持注入（调用方应丢弃，不要发 0 进去）。
//
// `mods` 是事件自带的修饰键。**小键盘**必须靠它区分：Qt 把小键盘数字的
// `key()` 也报成 `Key_0..9`，只有 `Qt::KeypadModifier` 能说明「这是小键盘」。
// 不区分的话，被控端 NumLock 状态与本地不一致时会打出完全不同的字符。
std::uint16_t qtKeyToVirtualKey(int qtKey, Qt::KeyboardModifiers mods = Qt::NoModifier);

// 该键是否属于「扩展键」（Win32 KEYEVENTF_EXTENDEDKEY）。
// 方向键 / Home / End / Insert / Delete / PageUp / PageDown / 小键盘除号 等需要置位，
// 否则被控端上的方向键会被当成小键盘数字、小键盘除号会变成主键盘斜杠。
bool qtKeyIsExtended(int qtKey, Qt::KeyboardModifiers mods = Qt::NoModifier);

// 一次按键该发给被控端的东西：虚拟键码 + 是否扩展键。
struct KeyMapping
{
    std::uint16_t vk       = 0;
    bool          extended = false;
};

// 由 QKeyEvent 的三项实参算出键位映射 —— 与上面两个函数的关系是**多认了左右**。
//
// ⚠️ 左右分不出来是 `nativeVirtualKey()` 的锅：真键盘按下左 Ctrl 与右 Ctrl，
//    `nativeVirtualKey()` **都**是 0x0011（Alt 都是 0x0012、Win 都是 0x005B），
//    拿它判左右永远判不出来（实测）。
//    可靠信号只有 `nativeScanCode()`：右侧 Ctrl / Alt / Win 带 0xE0 前缀。
//
// 右侧的处理方式（均经实机注入验证）：
//   · 右 Ctrl / 右 Alt：仍发通用码，但**必须**置扩展位（裸发会落到左边）；
//   · 右 Win：不能用「VK_LWIN + 扩展位」糊（实测仍是左 Win），改发 VK_RWIN；
//   · 右 Shift：不是扩展键（扫描码 0x36，没有 0xE0 前缀），改发 VK_RSHIFT，
//     加扩展位没用；
//   · Qt::Key_AltGr（部分布局把右 Alt 报成它）：按右 Alt 处理，别丢弃。
//
// ⚠️ 抬起必须原样带回按下时的 `extended`，否则右键会被抬成左键、永久卡住。
KeyMapping mapKeyEvent(int qtKey, Qt::KeyboardModifiers mods, quint32 nativeScanCode);

// `nativeScanCode` 取自 QKeyEvent::nativeScanCode()；0 表示「不知道」（测试里没给），
// 此时一律按左侧处理 —— 与修复前的行为一致。

} // namespace lanrdp
