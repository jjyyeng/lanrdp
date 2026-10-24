#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace lanrdp {

// 一次传输的分块大小：64 KiB。单块 Frame 载荷 ≈ 12 + 65536 字节。
constexpr int kFileChunkSize = 64 * 1024;

// 文件摘要固定 32 字节（SHA-256）
constexpr int kSha256Size = 32;

// 各载荷的固定段长度
constexpr std::size_t kFileHeaderFixedSize = 4 + 2 + 8;      // transferId + nameLen + totalSize
constexpr std::size_t kFileChunkHeaderSize = 4 + 8;          // transferId + offset
constexpr std::size_t kFileEndSize         = 4 + 8 + kSha256Size;

// FILE_HEADER（type=7）：一次传输的开场，声明文件名与总长度。
struct FileHeader
{
    std::uint32_t transferId = 0;
    QString       name;      // 仅文件名，不含目录
    std::uint64_t totalSize  = 0;
};

// FILE_CHUNK（type=8）：一块文件数据。
struct FileChunk
{
    std::uint32_t transferId = 0;
    std::uint64_t offset     = 0;  // 该块在文件中的起始偏移
    QByteArray    data;
};

// FILE_END（type=9）：收尾，带整文件 sha256 供接收端校验。
struct FileEnd
{
    std::uint32_t transferId = 0;
    std::uint64_t bytesSent  = 0;
    QByteArray    sha256;  // 必须恰为 kSha256Size 字节
};

QByteArray                encodeFileHeader(const FileHeader &header);
std::optional<FileHeader> decodeFileHeader(const QByteArray &payload);

QByteArray               encodeFileChunk(const FileChunk &chunk);
std::optional<FileChunk> decodeFileChunk(const QByteArray &payload);

QByteArray             encodeFileEnd(const FileEnd &end);
std::optional<FileEnd> decodeFileEnd(const QByteArray &payload);

// 把对端给的文件名清洗成可安全落盘的纯文件名：去掉目录部分与非法字符，
// 处理 Windows 保留设备名与结尾点号；无法清洗时回退为 "file"。
// 接收端必须调用——对端可以发来任意文件名，包括 "../../evil.txt"。
QString sanitizeFileName(const QString &name);

// 重名时追加 -1 / -2 …（扩展名保留在末尾），exists 返回 true 表示该名已被占用。
QString uniquifyFileName(const QString &name,
                         const std::function<bool(const QString &)> &exists);

// 单段数据的 sha256。大文件请用 QCryptographicHash 流式累加，勿整读进内存。
QByteArray sha256Of(const QByteArray &data);

} // namespace lanrdp
