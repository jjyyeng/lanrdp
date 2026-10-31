#include "viewer/AudioJitterBuffer.h"

#include <QMutexLocker>

namespace lanrdp {

void AudioJitterBuffer::configure(const AudioFormat &format)
{
    QMutexLocker lock(&mutex_);

    blockAlign_ = audioBlockAlign(format);
    // 每毫秒的字节数由「采样率 × 每帧字节数」换算，深度阈值才有时间含义。
    bytesPerMs_ = blockAlign_ > 0 ? static_cast<qsizetype>(format.sampleRate) * blockAlign_ / 1000 : 0;
    targetBytes_ = bytesForMs(kTargetMs);
    maxBytes_    = bytesForMs(kMaxBufferedMs);

    blocks_.clear();
    buffered_     = 0;
    droppedBytes_ = 0;
    underruns_    = 0;
}

qsizetype AudioJitterBuffer::bytesForMs(int ms) const
{
    // 格式非法时 configure 已经把这两项置 0 ⇒ push 会一直丢、take 会一直补静音：
    // 安全降级，不崩。
    return bytesPerMs_ * ms;
}

void AudioJitterBuffer::push(const QByteArray &pcm)
{
    if (pcm.isEmpty())
        return;

    QMutexLocker lock(&mutex_);

    blocks_.push_back(pcm);
    buffered_ += pcm.size();

    // 超上限 ⇒ 从队首丢到**目标深度**（不是「刚好不超上限」：
    // 后者会把延迟永久钉在上限上，越听越滞后）。
    // 按字节粒度裁剪而不是整块丢 —— 一次 push 可能是好几块粘在一起的。
    while (buffered_ > maxBytes_ && !blocks_.empty())
    {
        const qsizetype excess = buffered_ - targetBytes_;
        QByteArray     &front  = blocks_.front();
        if (front.size() <= excess)
        {
            buffered_ -= front.size();
            droppedBytes_ += front.size();
            blocks_.pop_front();
        }
        else
        {
            front.remove(0, excess);
            buffered_ -= excess;
            droppedBytes_ += excess;
        }
    }
}

AudioJitterBuffer::TakeResult AudioJitterBuffer::take(qsizetype bytes)
{
    TakeResult result;

    QMutexLocker lock(&mutex_);

    // 请求长度向上对齐到整帧：半帧进设备就是错位噪声。
    qsizetype want = bytes;
    if (blockAlign_ > 0 && want % blockAlign_ != 0)
        want += blockAlign_ - (want % blockAlign_);
    if (want <= 0)
        return result;

    result.data.reserve(want);

    while (result.data.size() < want && !blocks_.empty())
    {
        const qsizetype need = want - result.data.size();
        QByteArray     &front = blocks_.front();
        if (front.size() <= need)
        {
            result.data.append(front);
            buffered_ -= front.size();
            blocks_.pop_front();
        }
        else
        {
            result.data.append(front.constData(), need);
            front.remove(0, need);
            buffered_ -= need;
        }
    }

    if (result.data.size() < want)
    {
        result.data.append(want - result.data.size(), '\0');
        result.underrun = true;
        ++underruns_;
    }

    return result;
}

void AudioJitterBuffer::clear()
{
    QMutexLocker lock(&mutex_);

    blocks_.clear();
    buffered_     = 0;
    droppedBytes_ = 0;
    underruns_    = 0;
}

qsizetype AudioJitterBuffer::bufferedBytes() const
{
    QMutexLocker lock(&mutex_);
    return buffered_;
}

qsizetype AudioJitterBuffer::droppedBytes() const
{
    QMutexLocker lock(&mutex_);
    return droppedBytes_;
}

quint64 AudioJitterBuffer::underruns() const
{
    QMutexLocker lock(&mutex_);
    return underruns_;
}

bool AudioJitterBuffer::isEmpty() const
{
    QMutexLocker lock(&mutex_);
    return buffered_ == 0;
}

} // namespace lanrdp
