#include "protocol/H264Frame.h"

#include "protocol/LE.h"

namespace lanrdp {

namespace {

constexpr quint8 kFlagKeyframe = 1u << 0;

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
    le::appendU32(out, frame.width);
    le::appendU32(out, frame.height);
    out.append(static_cast<char>(frame.keyframe ? kFlagKeyframe : 0u));
    out.append(frame.bitstream);
    return out;
}

std::optional<H264Frame> decodeH264Frame(const QByteArray &payload)
{
    if (payload.size() <= kH264FrameHeaderSize)
        return std::nullopt;

    const quint32 width  = le::readU32(payload, 0);
    const quint32 height = le::readU32(payload, 4);

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
