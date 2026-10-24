#pragma once

#include <QByteArray>
#include <QImage>

namespace lanrdp {

// SCREEN_FRAME_RAW（type=4）的载荷布局：
// [width : u32 little-endian][height : u32 little-endian][像素数据]
// 像素为 32 位 BGRA、行紧密排列（每像素 4 字节，顺序 B,G,R,A），
// 与 QImage::Format_ARGB32 在小端机器上的内存布局一致。
constexpr int kRawFrameHeaderSize = 8;

// 将 QImage 编码为 SCREEN_FRAME_RAW 载荷；空图返回空数组。
QByteArray encodeRawFrame(const QImage &image);

// 从载荷还原 QImage（Format_ARGB32）；载荷非法/截断返回空 QImage。
QImage decodeRawFrame(const QByteArray &payload);

} // namespace lanrdp
