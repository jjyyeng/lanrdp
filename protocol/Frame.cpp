#include "protocol/Frame.h"

namespace lanrdp {

namespace {

inline std::uint8_t byteAt(const QByteArray &d, qsizetype i)
{
    return static_cast<std::uint8_t>(d.at(i));
}

} // namespace

QByteArray Frame::serialize() const
{
    // 超限返回空 = 拒发。帧头里的长度是 32 位，硬转会写出一个「长度字段小于
    // 实际内容」的帧：对端按声明长度切出一帧，剩下的字节被当成下一帧的魔数，
    // 解析立刻失败、连接被判流损坏。宁可这一帧不发。
    if (payload.size() > static_cast<qsizetype>(kMaxPayloadLen))
        return QByteArray();

    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());

    QByteArray out;
    out.reserve(static_cast<int>(kHeaderSize) + payload.size());

    // Magic[2]：'L','R'（0x4C52 big-endian）
    out.append(static_cast<char>((kMagic >> 8) & 0xFF));
    out.append(static_cast<char>(kMagic & 0xFF));
    // Version[1]
    out.append(static_cast<char>(version));
    // Type[1]
    out.append(static_cast<char>(type));
    // PayloadLen[4]：little-endian
    out.append(static_cast<char>(len & 0xFF));
    out.append(static_cast<char>((len >> 8) & 0xFF));
    out.append(static_cast<char>((len >> 16) & 0xFF));
    out.append(static_cast<char>((len >> 24) & 0xFF));
    // Payload
    out.append(payload);

    return out;
}

ParseStatus Frame::tryParse(const QByteArray &data, Frame *out, qsizetype *consumed,
                            qsizetype offset)
{
    if (offset < 0 || offset > data.size())
        return ParseStatus::Invalid;

    // 剩余可读长度（从 offset 起算）
    const qsizetype avail = data.size() - offset;
    if (avail < static_cast<qsizetype>(kHeaderSize))
        return ParseStatus::Incomplete;

    // Magic[2]：big-endian 读回
    const std::uint16_t magic =
        static_cast<std::uint16_t>((std::uint16_t(byteAt(data, offset + 0)) << 8)
                                   | byteAt(data, offset + 1));
    if (magic != kMagic)
        return ParseStatus::Invalid;

    const std::uint8_t version = byteAt(data, offset + 2);
    if (version != kVersion)
        return ParseStatus::Invalid;

    // PayloadLen[4]：little-endian
    const std::uint32_t len = static_cast<std::uint32_t>(byteAt(data, offset + 4))
                            | (static_cast<std::uint32_t>(byteAt(data, offset + 5)) << 8)
                            | (static_cast<std::uint32_t>(byteAt(data, offset + 6)) << 16)
                            | (static_cast<std::uint32_t>(byteAt(data, offset + 7)) << 24);

    // 荒唐长度一律判 Invalid（= 流已损坏，调用方清缓冲 + 断开），
    // 绝不放它进入 Incomplete —— 否则未认证对端可用一个假头让缓冲无限增长。
    if (len > kMaxPayloadLen)
        return ParseStatus::Invalid;

    const qsizetype total = static_cast<qsizetype>(kHeaderSize) + static_cast<qsizetype>(len);
    if (avail < total)
        return ParseStatus::Incomplete;

    if (out) {
        out->version = version;
        out->type    = byteAt(data, offset + 3);
        out->payload = data.mid(offset + static_cast<qsizetype>(kHeaderSize),
                                static_cast<qsizetype>(len));
    }
    if (consumed)
        *consumed = total;

    return ParseStatus::Ok;
}

std::optional<Frame> Frame::parse(const QByteArray &data)
{
    Frame f;
    qsizetype consumed = 0;
    if (tryParse(data, &f, &consumed) == ParseStatus::Ok)
        return f;
    return std::nullopt;
}

} // namespace lanrdp
