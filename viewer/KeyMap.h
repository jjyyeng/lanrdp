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

} // namespace lanrdp
