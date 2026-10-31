#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QVector>

#include <optional>

namespace lanrdp {

// 音频两帧的线上格式（协议 v4，帧类型 17 / 18）。
//
// 为什么值得一张配置帧：蓝牙耳机常是 44.1 kHz、部分声卡 96 kHz，把 48 kHz 写死在
// 两端就等于「换台机器就没声音」。配置帧让**采样率与声道成为线上事实**，
// 播放端只照它建渲染 client。
//
// 线上 PCM 恒为 **S16LE 交织**（每帧 channels × 2 字节）；两端都靠
// `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` 让 Windows 做重采样与格式转换，
// 因此整条链路里没有一行自研的重采样代码。

// 配置帧载荷长度：ver[1] + rate[4] + channels[1] + bits[1] + blockMs[2]
constexpr qsizetype kAudioConfigSize     = 9;
// 数据帧头长度：时间戳 u64（捕获相对毫秒，**仅诊断**，不参与播放调度）
constexpr qsizetype kAudioDataHeaderSize = 8;

constexpr quint8  kAudioConfigVersion = 1;
constexpr quint8  kAudioBitsPerSample = 16;
constexpr quint32 kAudioMinSampleRate = 8000;
constexpr quint32 kAudioMaxSampleRate = 192000;
constexpr quint8  kAudioMaxChannels   = 8;
constexpr quint16 kAudioMinBlockMs    = 5;
constexpr quint16 kAudioMaxBlockMs    = 100;

struct AudioFormat
{
    quint32 sampleRate    = 48000;
    quint8  channels      = 2;
    quint8  bitsPerSample = kAudioBitsPerSample;
    quint16 blockMs       = 20;
};

struct AudioChunk
{
    qint64     tsMs = 0;
    QByteArray pcm;
};

// 每帧（一个采样点上的全部声道）的字节数 = channels × 2。
qint64 audioBlockAlign(const AudioFormat &format);

// 一块（blockMs）PCM 的字节数。格式非法时返回 0。
qint64 audioBlockBytes(const AudioFormat &format);

// 字段范围检查：采样率 / 声道 / 位深 / 块长任一越界即 false。
// 线上字段一律不可信 —— 解码侧拿它分配缓冲，越界就是巨额分配。
bool isValidAudioFormat(const AudioFormat &format);

QByteArray encodeAudioConfig(const AudioFormat &format);
std::optional<AudioFormat> decodeAudioConfig(const QByteArray &payload);

QByteArray encodeAudioData(qint64 tsMs, const QByteArray &pcm);

// 解数据帧。`format` 非空时**额外要求 PCM 是本格式整帧的整数倍**：
// 非整帧长度（哪怕只是尾部多 2 字节）进了播放端的字节队列，后面每一块都会被
// 平移半个采样 —— 听感是「整条音轨永久错位」，比丢一块难查得多。
// 播放端只按 `availFrames = availableBytes / blockAlign` 向下取整，剩的尾数会
// 永久留在队首，所以必须在**入口**拒掉。传 nullptr = 还不知道格式（只查偶数长度）。
std::optional<AudioChunk> decodeAudioData(const QByteArray &payload,
                                          const AudioFormat *format = nullptr);

// 把采集端拿到的零散字节攒成定长块。
//
// 为什么需要它：WASAPI 的包大小由设备周期决定（本机实测 480 帧 ≈ 10 ms），
// 与 20 ms 不成整数倍；不去攒就会出现块长逐块抖动，播放端的缓冲深度跟着抖。
class AudioBlockAssembler
{
public:
    void    setBlockBytes(qint64 bytes);   // <= 0 ⇒ 未配置，永不出块
    qint64  blockBytes() const { return blockBytes_; }
    qint64  pendingBytes() const { return pending_.size(); }

    // 追加一段字节，返回本次成形的**完整块**（0..n 个）。不足一块的尾巴留在内部。
    QVector<QByteArray> append(const char *data, qint64 len);

    // 只丢尾巴。停机时的残留（< 一块）没有发送价值：连接正在关，多送 20 ms 的
    // 尾巴换不来任何听感，反而要多一条“最后一块”的特殊路径。
    void clear();

private:
    QByteArray pending_;
    qint64     blockBytes_ = 0;
};

} // namespace lanrdp

Q_DECLARE_METATYPE(lanrdp::AudioFormat)
