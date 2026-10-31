#include "protocol/AudioChunk.h"

namespace lanrdp {

namespace {

void appendU16LE(QByteArray &out, quint16 value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32LE(QByteArray &out, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

void appendU64LE(QByteArray &out, quint64 value)
{
    for (int i = 0; i < 8; ++i)
        out.append(static_cast<char>((value >> (8 * i)) & 0xFFu));
}

quint8 readU8(const QByteArray &data, qsizetype offset)
{
    return static_cast<quint8>(data.at(offset));
}

quint16 readU16LE(const QByteArray &data, qsizetype offset)
{
    return static_cast<quint16>(readU8(data, offset))
           | static_cast<quint16>(static_cast<quint16>(readU8(data, offset + 1)) << 8);
}

quint32 readU32LE(const QByteArray &data, qsizetype offset)
{
    quint32 value = 0;
    for (int i = 0; i < 4; ++i)
        value |= static_cast<quint32>(readU8(data, offset + i)) << (8 * i);
    return value;
}

quint64 readU64LE(const QByteArray &data, qsizetype offset)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<quint64>(readU8(data, offset + i)) << (8 * i);
    return value;
}

} // namespace

qint64 audioBlockAlign(const AudioFormat &format)
{
    if (!isValidAudioFormat(format))
        return 0;
    return static_cast<qint64>(format.channels) * (format.bitsPerSample / 8);
}

qint64 audioBlockBytes(const AudioFormat &format)
{
    const qint64 align = audioBlockAlign(format);
    if (align <= 0)
        return 0;
    // 先按毫秒算采样帧数，再乘每帧字节数。取整误差最多半个采样点，
    // 与设备周期无关（块长只用于「攒多少发一次」，不用于时间基准）。
    const qint64 frames = static_cast<qint64>(format.sampleRate) * format.blockMs / 1000;
    return frames * align;
}

bool isValidAudioFormat(const AudioFormat &format)
{
    if (format.sampleRate < kAudioMinSampleRate || format.sampleRate > kAudioMaxSampleRate)
        return false;
    if (format.channels == 0 || format.channels > kAudioMaxChannels)
        return false;
    if (format.bitsPerSample != kAudioBitsPerSample)
        return false;
    if (format.blockMs < kAudioMinBlockMs || format.blockMs > kAudioMaxBlockMs)
        return false;
    return true;
}

QByteArray encodeAudioConfig(const AudioFormat &format)
{
    QByteArray out;
    out.reserve(kAudioConfigSize);
    out.append(static_cast<char>(kAudioConfigVersion));
    appendU32LE(out, format.sampleRate);
    out.append(static_cast<char>(format.channels));
    out.append(static_cast<char>(format.bitsPerSample));
    appendU16LE(out, format.blockMs);
    return out;
}

std::optional<AudioFormat> decodeAudioConfig(const QByteArray &payload)
{
    // 长度必须精确匹配：多出来的字节说明对端与本端的布局认知已经不一致了。
    if (payload.size() != kAudioConfigSize)
        return std::nullopt;
    if (readU8(payload, 0) != kAudioConfigVersion)
        return std::nullopt;

    AudioFormat format;
    format.sampleRate    = readU32LE(payload, 1);
    format.channels      = readU8(payload, 5);
    format.bitsPerSample = readU8(payload, 6);
    format.blockMs       = readU16LE(payload, 7);

    if (!isValidAudioFormat(format))
        return std::nullopt;
    return format;
}

QByteArray encodeAudioData(qint64 tsMs, const QByteArray &pcm)
{
    QByteArray out;
    out.reserve(kAudioDataHeaderSize + pcm.size());
    appendU64LE(out, static_cast<quint64>(tsMs));
    out.append(pcm);
    return out;
}

std::optional<AudioChunk> decodeAudioData(const QByteArray &payload, const AudioFormat *format)
{
    if (payload.size() < kAudioDataHeaderSize)
        return std::nullopt;

    const qsizetype pcmBytes = payload.size() - kAudioDataHeaderSize;
    // 16 位交织 ⇒ PCM 长度必为偶数。奇数说明发送端边界已经错了，
    // 照它解出来是半字节错位的噪声 —— 丢一块比放噪声强。
    if (pcmBytes % 2 != 0)
        return std::nullopt;

    // 知道格式时再查一层：必须是整帧的整数倍。否则播放端的字节队列会永久留着
    // 一个 2 字节的尾数，之后每取一块都整体平移半个采样 —— 越听越歪，且不会自愈。
    if (format)
    {
        const qint64 align = audioBlockAlign(*format);
        if (align > 0 && pcmBytes % align != 0)
            return std::nullopt;
    }

    AudioChunk chunk;
    chunk.tsMs = static_cast<qint64>(readU64LE(payload, 0));
    chunk.pcm  = payload.mid(kAudioDataHeaderSize);
    return chunk;
}

void AudioBlockAssembler::setBlockBytes(qint64 bytes)
{
    blockBytes_ = bytes;
    if (blockBytes_ <= 0)
        pending_.clear();
}

QVector<QByteArray> AudioBlockAssembler::append(const char *data, qint64 len)
{
    QVector<QByteArray> blocks;
    if (!data || len <= 0 || blockBytes_ <= 0)
        return blocks;

    pending_.append(data, len);

    // 先按步长扫一遍切出来，最后**只压缩一次**剩余尾巴 ——
    // 每切一块就 remove(0, n) 的话，一次大块追加（设备缓冲攒了好几拍）
    // 会退化成 O(n²) 的前移。
    qsizetype offset  = 0;
    const auto total = pending_.size();
    while (total - offset >= blockBytes_)
    {
        blocks.append(pending_.mid(offset, blockBytes_));
        offset += blockBytes_;
    }
    if (offset > 0)
        pending_.remove(0, offset);
    return blocks;
}

void AudioBlockAssembler::clear()
{
    pending_.clear();
}

} // namespace lanrdp
