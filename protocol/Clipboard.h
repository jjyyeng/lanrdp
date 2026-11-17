#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

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

// ---------------------------------------------------------------------------
// CLIPBOARD_FILES（v6）：剪贴板里放的是**文件**（资源管理器里 Ctrl+C）。
//
// 为什么需要这一帧：文件本身走 FILE_* 三帧传，但那三帧只说明「有个文件要落盘」，
// 落完就结束了。要还原「对方复制了文件、我这边能直接粘贴」这件事，
// 接收侧必须知道**这批文件属于同一次复制**、并且一共几个。
// 没有它，收到 3 个文件就只能显示三行「文件接收完成」，剪贴板里什么也没有。
//
// 载荷布局（小端）：
//   u16 count | count × (u16 nameLen + nameLen 字节 UTF-8 文件名)
// 名字只用于提示与日志（落盘路径由 FILE_HEADER 决定），所以只带文件名、
// 不带目录 —— 与 FileTransfer 的约定一致（接收侧只认基名）。
// ---------------------------------------------------------------------------

// 一次复制最多带多少个文件。超过就整批不发（发一半等于让对方粘贴出残缺的一批）。
constexpr int kClipboardMaxFiles = 64;

// 单个文件名的字节上限。窗口系统给的文件名远小于此，这里只是给解码侧一个硬边界。
constexpr std::size_t kClipboardMaxFileNameBytes = 255;

struct ClipboardFiles
{
    QStringList names;
};

// 编码 CLIPBOARD_FILES 载荷。空清单 / 超过 kClipboardMaxFiles / 某个名字为空或超长
// 时返回 nullopt（调用方应拒绝发送并给出理由）。
std::optional<QByteArray> encodeClipboardFiles(const QStringList &names);

// 解码 CLIPBOARD_FILES 载荷。长度不自洽、count 越界、名字长度越界都返回 nullopt。
std::optional<ClipboardFiles> decodeClipboardFiles(const QByteArray &payload);

} // namespace lanrdp
