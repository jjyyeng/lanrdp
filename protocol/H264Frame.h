#pragma once

#include <QByteArray>

#include <cstdint>
#include <optional>

namespace lanrdp {

// SCREEN_FRAME_H264（type=6）载荷布局，全部小端：
// [width u32][height u32][flags u8][Annex-B 码流...]
// flags 的 bit0 表示关键帧——该帧自带 SPS/PPS/IDR，解码器可由此帧起独立解码。
//
// 码流长度不单独存：外层 Frame 的 PayloadLen 已经精确定界，
// 本层只需防御头部本身非法的情况。
constexpr int kH264FrameHeaderSize = 9;

// 单帧允许的最大边长；超出即视为伪造载荷，避免被骗出巨额分配。
constexpr quint32 kH264MaxDimension = 8192;

struct H264Frame
{
    quint32    width     = 0;
    quint32    height    = 0;
    bool       keyframe  = false;
    QByteArray bitstream;   // Annex-B：以 00 00 00 01 起始码分隔的 NALU
};

// 编码为 SCREEN_FRAME_H264 载荷；尺寸为 0、超上限或码流为空时返回空数组。
QByteArray encodeH264Frame(const H264Frame &frame);

// 载荷非法（过短、尺寸为 0 或超上限、没有码流）时返回 nullopt。
std::optional<H264Frame> decodeH264Frame(const QByteArray &payload);

} // namespace lanrdp
