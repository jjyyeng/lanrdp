#include "protocol/ScreenFrame.h"

#include <cstring>

namespace lanrdp {

namespace {

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

QByteArray encodeRawFrame(const QImage &image)
{
    if (image.isNull())
        return {};

    const QImage source = (image.format() == QImage::Format_ARGB32)
                              ? image
                              : image.convertToFormat(QImage::Format_ARGB32);

    const int width    = source.width();
    const int height   = source.height();
    const int rowBytes = width * 4;

    QByteArray out;
    out.reserve(kRawFrameHeaderSize + static_cast<qsizetype>(rowBytes) * height);
    appendU32LE(out, static_cast<quint32>(width));
    appendU32LE(out, static_cast<quint32>(height));

    for (int y = 0; y < height; ++y)
        out.append(reinterpret_cast<const char *>(source.constScanLine(y)), rowBytes);

    return out;
}

QImage decodeRawFrame(const QByteArray &payload)
{
    if (payload.size() < kRawFrameHeaderSize)
        return {};

    const quint32 width  = readU32LE(payload, 0);
    const quint32 height = readU32LE(payload, 4);

    // 防御异常尺寸，避免被伪造载荷骗出巨额分配
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
        return {};

    const qsizetype rowBytes  = static_cast<qsizetype>(width) * 4;
    const qsizetype pixelBytes = rowBytes * static_cast<qsizetype>(height);
    if (payload.size() - kRawFrameHeaderSize < pixelBytes)
        return {};

    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32);
    // 构造也可能失败（内存压力 / 尺寸过大被 Qt 拒绝）—— 此时 image 是 null，
    // scanLine() 会返回空指针，下面的 memcpy 直接崩。宁可返回空图让上层跳过本帧。
    if (image.isNull())
        return {};

    const char *src = payload.constData() + kRawFrameHeaderSize;

    for (quint32 y = 0; y < height; ++y) {
        std::memcpy(image.scanLine(static_cast<int>(y)),
                    src + static_cast<qsizetype>(y) * rowBytes,
                    static_cast<std::size_t>(rowBytes));
    }

    return image;
}

} // namespace lanrdp
