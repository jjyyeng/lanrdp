#pragma once

#include <QByteArray>
#include <QtGlobal>

// 协议载荷的小端读写，全项目唯一一份。
//
// 字节序和字段布局是线上格式的一部分：改这里等于改协议，两端必须同步升级。
// 两种读取并存是有意的 ——
//   readUxx  ：调用方**已经**校验过总长度，走它最省（帧内定长头解析）；
//   tryReadUxx：长度来自网络、没法提前确定时用，越界返回 false 而不是读飞。
namespace lanrdp::le {

// ---------------------------------------------------------------- 写入
// 一律追加到 QByteArray 尾部，小端。

inline void appendU8(QByteArray &out, quint8 value)
{
    out.append(static_cast<char>(value));
}

inline void appendU16(QByteArray &out, quint16 value)
{
    for (int i = 0; i < 2; ++i)
        out.append(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

inline void appendU32(QByteArray &out, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

inline void appendU64(QByteArray &out, quint64 value)
{
    for (int i = 0; i < 8; ++i)
        out.append(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

// 有符号值按补码原样写出：负数（屏在主屏左/上方的坐标）靠这个往返。
inline void appendI32(QByteArray &out, qint32 value)
{
    appendU32(out, static_cast<quint32>(value));
}

// ---------------------------------------------------------------- 读取
// 前置条件：offset + 字段宽 <= data.size()。调用方负责守长度。

inline quint8 readU8(const QByteArray &data, qsizetype offset)
{
    return static_cast<quint8>(data.at(offset));
}

inline quint16 readU16(const QByteArray &data, qsizetype offset)
{
    return static_cast<quint16>(readU8(data, offset))
           | static_cast<quint16>(static_cast<quint16>(readU8(data, offset + 1)) << 8);
}

inline quint32 readU32(const QByteArray &data, qsizetype offset)
{
    quint32 value = 0;
    for (int i = 0; i < 4; ++i)
        value |= static_cast<quint32>(readU8(data, offset + i)) << (8 * i);
    return value;
}

inline quint64 readU64(const QByteArray &data, qsizetype offset)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<quint64>(readU8(data, offset + i)) << (8 * i);
    return value;
}

inline qint32 readI32(const QByteArray &data, qsizetype offset)
{
    return static_cast<qint32>(readU32(data, offset));
}

// -------------------------------------------------------- 带守卫的读取
// 越界返回 false 且不写 *out —— 直接吃网络来的偏移量时只许用这一组。

inline bool tryReadU16(const QByteArray &data, qsizetype offset, quint16 *out)
{
    if (!out || offset < 0 || offset + 2 > data.size())
        return false;
    *out = readU16(data, offset);
    return true;
}

inline bool tryReadU32(const QByteArray &data, qsizetype offset, quint32 *out)
{
    if (!out || offset < 0 || offset + 4 > data.size())
        return false;
    *out = readU32(data, offset);
    return true;
}

} // namespace lanrdp::le
