#include "protocol/Clipboard.h"

namespace lanrdp {

std::optional<QByteArray> encodeClipboardText(const QString &text)
{
    // 先用 UTF-16 长度做一个**下界**判断（UTF-8 每个 UTF-16 单元至少 1 字节）。
    // 先转换再判上限的话，一个几百 MB 的本地剪贴板会在 UI 线程先被完整复制一份
    // UTF-8（最大 4×）再被拒绝 —— 而那份内容根本不会发出去。
    if (static_cast<std::size_t>(text.size()) > kClipboardMaxBytes)
        return std::nullopt;

    const QByteArray utf8 = text.toUtf8();
    if (static_cast<std::size_t>(utf8.size()) > kClipboardMaxBytes)
        return std::nullopt;

    return utf8;
}

std::optional<ClipboardText> decodeClipboardText(const QByteArray &payload)
{
    if (static_cast<std::size_t>(payload.size()) > kClipboardMaxBytes)
        return std::nullopt;

    ClipboardText out;
    out.text = QString::fromUtf8(payload);
    return out;
}

} // namespace lanrdp
