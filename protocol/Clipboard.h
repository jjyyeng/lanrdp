#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <optional>

namespace lanrdp {

// 剪贴板纯文本单帧上限（按 UTF-8 字节数计）。
// 单帧不宜过大，避免一次性吃掉带宽；超限直接丢弃并在日志里说明原因。
constexpr std::size_t kClipboardMaxBytes = 1024u * 1024u;  // 1 MiB

// 解码结果。text 允许为空串（语义 = 清空剪贴板）。
//
// 载荷**没有**任何头部字段 —— 就是裸 UTF-8 文本。
// 从前这里带一个 u32 的 `seq` 序号，但它自上线起就没参与过排序或陈旧判定
// （解码后当场丢弃，只在测试里被读过），"看起来像有防护"反而误导维护者。
// 去掉它也就去掉了「谁能用这个序号做什么」的想象空间：TCP 本身有序，
// 单条连接内不存在乱序；跨会话的陈旧回放属于鉴权/TLS 那一层的事。
// ⚠️ 载荷布局变了 ⇒ 协议版本随之升到 v3，两端必须同版本。
struct ClipboardText
{
    QString text;
};

// 编码 CLIPBOARD_TEXT 载荷 = UTF-8 文本。
// 文本的 UTF-8 字节数超过 kClipboardMaxBytes 时返回 nullopt（调用方应拒绝发送）。
std::optional<QByteArray> encodeClipboardText(const QString &text);

// 解码 CLIPBOARD_TEXT 载荷。返回 nullopt 的唯一原因是字节数超过上限。
// 空载荷是合法的（= 空文本），非法 UTF-8 字节由 QString::fromUtf8 容错处理，不算失败。
std::optional<ClipboardText> decodeClipboardText(const QByteArray &payload);

} // namespace lanrdp
