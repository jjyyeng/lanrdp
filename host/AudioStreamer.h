#pragma once

#include "protocol/AudioChunk.h"

#include <QObject>
#include <QVector>

#include <functional>

namespace lanrdp {

class Session;

// 被控端的音频扇出：一块 PCM 送给所有已鉴权的会话。
//
// 它**不采集**（采集在 `host/AudioCapturer`，跑在自己的线程里）、也**不编码**
// （线上就是 S16LE）。本类只做三件事，三件都容易写错，所以单独成类并可单测：
//   ① 目标管理（懒加 / 懒删）；
//   ② 格式变化时给每个目标补发一帧 AUDIO_CONFIG（新接入的目标也要补）；
//   ③ 逐目标判背压 —— `Session::sendFrame` 没有背压，写入会无上限堆在内存里；
//      音频一旦排队就是「越听越滞后」，所以过期即丢（只丢这一块，不断流）。
class AudioStreamer : public QObject
{
    Q_OBJECT

public:
    // 背压来源：默认读真实 socket 的 `pendingBytes()`，测试注入假值。
    using PendingSource = std::function<qint64(Session *)>;

    // 背压水位。音频每秒只有 192 KB，1 MiB 意味着「对端已经卡了 5 秒以上」；
    // 超过它就该丢块而不是继续追。
    static constexpr qint64 kDefaultBackpressureBytes = 1024 * 1024;

    explicit AudioStreamer(QObject *parent = nullptr);

    void setPendingSource(PendingSource source);
    void setBackpressureBytes(qint64 bytes);
    qint64 backpressureBytes() const { return backpressureBytes_; }

    void addTarget(Session *session);
    void removeTarget(Session *session);
    void clearTargets();
    int  targetCount() const { return targets_.size(); }

    // 送一块出去。格式变了就先把 AUDIO_CONFIG 补给所有目标。
    // 无目标时什么也不发（但仍记住格式 —— 下一个目标接入时能立刻收到配置）。
    void submit(const AudioFormat &format, qint64 tsMs, const QByteArray &pcm);

    AudioFormat format() const { return format_; }
    bool        hasFormat() const { return hasFormat_; }

    quint64 chunksSent() const { return chunksSent_; }      // 按「目标 × 块」计
    quint64 bytesSent() const { return bytesSent_; }
    quint64 chunksSkipped() const { return chunksSkipped_; } // 被背压挡掉的块数

private:
    void   sendConfigTo(Session *session);
    qint64 pendingOf(Session *session) const;
    bool   sameFormat(const AudioFormat &a, const AudioFormat &b) const;

    QVector<Session *> targets_;
    PendingSource      pendingSource_;

    qint64      backpressureBytes_ = kDefaultBackpressureBytes;
    AudioFormat format_;
    bool        hasFormat_ = false;

    quint64 chunksSent_    = 0;
    quint64 bytesSent_     = 0;
    quint64 chunksSkipped_ = 0;
};

} // namespace lanrdp
