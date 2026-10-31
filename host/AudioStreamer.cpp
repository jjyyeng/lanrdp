#include "host/AudioStreamer.h"

#include "net/Session.h"

#include <utility>

namespace lanrdp {

AudioStreamer::AudioStreamer(QObject *parent)
    : QObject(parent)
{
}

void AudioStreamer::setPendingSource(PendingSource source)
{
    pendingSource_ = std::move(source);
}

void AudioStreamer::setBackpressureBytes(qint64 bytes)
{
    if (bytes > 0)
        backpressureBytes_ = bytes;
}

bool AudioStreamer::sameFormat(const AudioFormat &a, const AudioFormat &b) const
{
    return a.sampleRate == b.sampleRate && a.channels == b.channels
           && a.bitsPerSample == b.bitsPerSample && a.blockMs == b.blockMs;
}

qint64 AudioStreamer::pendingOf(Session *session) const
{
    if (!session)
        return 0;
    if (pendingSource_)
        return pendingSource_(session);
    return session->pendingBytes();
}

void AudioStreamer::sendConfigTo(Session *session)
{
    if (!session || !hasFormat_)
        return;
    session->sendFrame(static_cast<std::uint8_t>(FrameType::AudioConfig),
                       encodeAudioConfig(format_));
}

void AudioStreamer::addTarget(Session *session)
{
    if (!session || targets_.contains(session))
        return;
    targets_.append(session);
    // 新目标要立刻知道格式：否则它得等到「格式下次变化」才建得起来渲染端，
    // 而格式可能半小时都不变。
    sendConfigTo(session);
}

void AudioStreamer::removeTarget(Session *session)
{
    targets_.removeAll(session);
}

void AudioStreamer::clearTargets()
{
    targets_.clear();
}

void AudioStreamer::submit(const AudioFormat &format, qint64 tsMs, const QByteArray &pcm)
{
    if (pcm.isEmpty() || !isValidAudioFormat(format))
        return;

    // 格式先落定（与有没有目标无关）：它描述的是**采集端**的事实，
    // 记住了才能让后接入的目标立刻收到配置。
    if (!hasFormat_ || !sameFormat(format_, format))
    {
        format_    = format;
        hasFormat_ = true;
        for (Session *target : std::as_const(targets_))
            sendConfigTo(target);
    }

    if (targets_.isEmpty())
        return;

    const QByteArray payload = encodeAudioData(tsMs, pcm);
    for (Session *target : std::as_const(targets_))
    {
        // 逐目标独立判：一个慢接收端不该拖累其他控制端（与视频扇出同规矩）。
        // 音频**不排队**：排队的音频就是延迟，过期即丢。
        if (pendingOf(target) > backpressureBytes_)
        {
            ++chunksSkipped_;
            continue;
        }
        target->sendFrame(static_cast<std::uint8_t>(FrameType::AudioData), payload);
        ++chunksSent_;
        bytesSent_ += static_cast<quint64>(payload.size());
    }
}

} // namespace lanrdp
