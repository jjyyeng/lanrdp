#include "protocol/Clipboard.h"

#include "protocol/LE.h"

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

std::optional<QByteArray> encodeClipboardFiles(const QStringList &names)
{
    if (names.isEmpty() || names.size() > kClipboardMaxFiles)
        return std::nullopt;

    QByteArray out;
    le::appendU16(out, static_cast<quint16>(names.size()));

    for (const QString &name : names) {
        const QByteArray utf8 = name.toUtf8();
        if (utf8.isEmpty() || static_cast<std::size_t>(utf8.size()) > kClipboardMaxFileNameBytes)
            return std::nullopt;
        le::appendU16(out, static_cast<quint16>(utf8.size()));
        out.append(utf8);
    }
    return out;
}

std::optional<ClipboardFiles> decodeClipboardFiles(const QByteArray &payload)
{
    quint16 count = 0;
    if (!le::tryReadU16(payload, 0, &count))
        return std::nullopt;
    if (count == 0 || count > kClipboardMaxFiles)
        return std::nullopt;

    ClipboardFiles out;
    out.names.reserve(count);

    qsizetype offset = 2;
    for (quint16 i = 0; i < count; ++i) {
        quint16 nameLen = 0;
        if (!le::tryReadU16(payload, offset, &nameLen))
            return std::nullopt;
        offset += 2;

        if (nameLen == 0 || static_cast<std::size_t>(nameLen) > kClipboardMaxFileNameBytes)
            return std::nullopt;
        if (offset + nameLen > payload.size())
            return std::nullopt;

        out.names.append(QString::fromUtf8(payload.constData() + offset, nameLen));
        offset += nameLen;
    }

    // 多出来的字节说明编码方与解码方的布局不一致 —— 宁可整帧丢弃也不要猜。
    if (offset != payload.size())
        return std::nullopt;

    return out;
}

} // namespace lanrdp
