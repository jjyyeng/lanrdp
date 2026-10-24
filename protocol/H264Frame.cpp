#include "protocol/H264Frame.h"

namespace lanrdp {

namespace {

constexpr quint8 kFlagKeyframe = 1u << 0;

void appendU32LE(QByteArray &out, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

quint32 readU32LE(const QByteArray &data, int offset)
{
    return (static_cast<quint32>(static_cast<quint8>(data.at(offset))))
           | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1))) << 8)
           | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2))) << 16)
           | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 3))) << 24);
}

} // namespace

QByteArray encodeH264Frame(const H264Frame &frame)
{
    if (frame.width == 0 || frame.height == 0)
        return {};
    if (frame.width > kH264MaxDimension || frame.height > kH264MaxDimension)
        return {};
    if (frame.bitstream.isEmpty())
        return {};

    QByteArray out;
    out.reserve(kH264FrameHeaderSize + frame.bitstream.size());
    appendU32LE(out, frame.width);
    appendU32LE(out, frame.height);
    out.append(static_cast<char>(frame.keyframe ? kFlagKeyframe : 0u));
    out.append(frame.bitstream);
    return out;
}

std::optional<H264Frame> decodeH264Frame(const QByteArray &payload)
{
    if (payload.size() <= kH264FrameHeaderSize)
        return std::nullopt;

    const quint32 width  = readU32LE(payload, 0);
    const quint32 height = readU32LE(payload, 4);

    if (width == 0 || height == 0)
        return std::nullopt;
    if (width > kH264MaxDimension || height > kH264MaxDimension)
        return std::nullopt;

    H264Frame frame;
    frame.width     = width;
    frame.height    = height;
    frame.keyframe  = (static_cast<quint8>(payload.at(8)) & kFlagKeyframe) != 0;
    frame.bitstream = payload.mid(kH264FrameHeaderSize);
    return frame;
}

} // namespace lanrdp
