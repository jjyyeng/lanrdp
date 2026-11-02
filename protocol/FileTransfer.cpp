#include "protocol/FileTransfer.h"

#include "protocol/LE.h"

#include <QCryptographicHash>
#include <QSet>

namespace lanrdp {

namespace {

// Windows 保留设备名：即使带扩展名（如 "CON.txt"）也不能直接落盘，
// 否则打开文件会异常甚至会挂住。
// ⚠️ Windows 判定时会**忽略基本名尾部的空格与点** —— "CON .txt" / "CON." 与 "CON"
//    等价，都是设备名。故比较前须按同一规则规范化，否则带空格的变体可绕过。
bool isReservedDeviceName(const QString &base)
{
    QString key = base;
    while (!key.isEmpty()
           && (key.endsWith(QLatin1Char(' ')) || key.endsWith(QLatin1Char('.'))))
        key.chop(1);
    if (key.isEmpty())
        return false;

    static const QSet<QString> kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),  QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9"),
    };
    return kReserved.contains(key.toUpper());
}

} // namespace

// ---------------------------------------------------------------------------
// FILE_HEADER
// ---------------------------------------------------------------------------

QByteArray encodeFileHeader(const FileHeader &header)
{
    const QByteArray name = header.name.toUtf8();
    if (name.isEmpty() || name.size() > 0xFFFF)
        return {};
    if (header.totalSize == 0)
        return {};

    QByteArray out;
    out.reserve(static_cast<int>(kFileHeaderFixedSize) + name.size());
    le::appendU32(out, header.transferId);
    le::appendU16(out, static_cast<quint16>(name.size()));
    out.append(name);
    le::appendU64(out, header.totalSize);
    return out;
}

std::optional<FileHeader> decodeFileHeader(const QByteArray &payload)
{
    if (static_cast<std::size_t>(payload.size()) < kFileHeaderFixedSize)
        return std::nullopt;

    const quint32 transferId = le::readU32(payload, 0);
    const quint16 nameLen    = le::readU16(payload, 4);
    if (nameLen == 0)
        return std::nullopt;

    // 长度必须严丝合缝：多一个字节都说明流被写歪了
    if (static_cast<std::size_t>(payload.size()) != kFileHeaderFixedSize + nameLen)
        return std::nullopt;

    const QString name = QString::fromUtf8(payload.mid(6, nameLen));
    if (name.isEmpty())
        return std::nullopt;

    const quint64 totalSize = le::readU64(payload, 6 + nameLen);
    if (totalSize == 0)
        return std::nullopt;

    FileHeader header;
    header.transferId = transferId;
    header.name       = name;
    header.totalSize  = totalSize;
    return header;
}

// ---------------------------------------------------------------------------
// FILE_CHUNK
// ---------------------------------------------------------------------------

QByteArray encodeFileChunk(const FileChunk &chunk)
{
    if (chunk.data.isEmpty())
        return {};

    QByteArray out;
    out.reserve(static_cast<int>(kFileChunkHeaderSize) + chunk.data.size());
    le::appendU32(out, chunk.transferId);
    le::appendU64(out, chunk.offset);
    out.append(chunk.data);
    return out;
}

std::optional<FileChunk> decodeFileChunk(const QByteArray &payload)
{
    // 必须严格大于头长度：只有头的空块无意义
    if (static_cast<std::size_t>(payload.size()) <= kFileChunkHeaderSize)
        return std::nullopt;

    // 块长必须落在发送端约定内（kFileChunkSize）。线上契约若不设上限，对端就能塞
    // 任意大的块进来 —— 帧层虽有 32 MiB 兜底，但那是「流已损坏」的门槛，不是「块」的
    // 语义约束；接收端会为超长块多付一次整块拷贝。
    if (payload.size() - static_cast<int>(kFileChunkHeaderSize) > kFileChunkSize)
        return std::nullopt;

    FileChunk chunk;
    chunk.transferId = le::readU32(payload, 0);
    chunk.offset     = le::readU64(payload, 4);
    chunk.data       = payload.mid(static_cast<int>(kFileChunkHeaderSize));
    return chunk;
}

// ---------------------------------------------------------------------------
// FILE_END
// ---------------------------------------------------------------------------

QByteArray encodeFileEnd(const FileEnd &end)
{
    if (end.sha256.size() != kSha256Size)
        return {};

    QByteArray out;
    out.reserve(static_cast<int>(kFileEndSize));
    le::appendU32(out, end.transferId);
    le::appendU64(out, end.bytesSent);
    out.append(end.sha256);
    return out;
}

std::optional<FileEnd> decodeFileEnd(const QByteArray &payload)
{
    if (static_cast<std::size_t>(payload.size()) != kFileEndSize)
        return std::nullopt;

    FileEnd end;
    end.transferId = le::readU32(payload, 0);
    end.bytesSent  = le::readU64(payload, 4);
    end.sha256     = payload.mid(12, kSha256Size);
    return end;
}

// ---------------------------------------------------------------------------
// 文件名处理
// ---------------------------------------------------------------------------

QString sanitizeFileName(const QString &name)
{
    QString s = name;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // 只取最后一段：对端可能送来 "../../evil.txt" 或 "C:\Windows\x.dll"
    const int slash = s.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        s = s.mid(slash + 1);

    static const QString kIllegal = QStringLiteral("/\\:*?\"<>|");
    QString              out;
    out.reserve(s.size());
    for (const QChar ch : s) {
        if (ch.unicode() < 0x20)
            continue;
        if (kIllegal.contains(ch))
            continue;
        out.append(ch);
    }

    // Windows 不允许文件名以点或空格结尾（会被静默吃掉）
    while (!out.isEmpty()
           && (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))))
        out.chop(1);
    out = out.trimmed();

    if (out.isEmpty())
        return QStringLiteral("file");

    const int     dot  = out.indexOf(QLatin1Char('.'));
    const QString base = (dot >= 0) ? out.left(dot) : out;
    if (isReservedDeviceName(base))
        return QStringLiteral("_") + out;

    return out;
}

QString uniquifyFileName(const QString &name,
                         const std::function<bool(const QString &)> &exists)
{
    if (!exists || !exists(name))
        return name;

    // "a.tar.gz" -> stem "a.tar" + suffix ".gz"，避免产出 "a.tar-1.gz" 之外的怪名
    const int     dot    = name.lastIndexOf(QLatin1Char('.'));
    const bool    hasExt = dot > 0;
    const QString stem   = hasExt ? name.left(dot) : name;
    const QString suffix = hasExt ? name.mid(dot) : QString();

    for (int i = 1; i < 10000; ++i) {
        const QString candidate = QStringLiteral("%1-%2%3").arg(stem).arg(i).arg(suffix);
        if (!exists(candidate))
            return candidate;
    }

    // 候选名耗尽 —— 返回**空**，由调用方转成「传输失败」。
    // 不能回落到入参原名：那个名字正是「已被占用」的那个，调用方若照它落盘
    // （WriteOnly|Truncate）就会清空用户已有的同名文件。
    return QString();
}

QByteArray sha256Of(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

} // namespace lanrdp
