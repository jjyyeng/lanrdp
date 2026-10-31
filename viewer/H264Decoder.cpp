#include "viewer/H264Decoder.h"

#include "host/ColorConvert.h"
#include "host/MfRuntime.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>

#include <cstring>
#include <cwchar>
#include <thread>

namespace lanrdp {

namespace {

IMFMediaType *firstTypeMatching(IMFTransform *mft, bool input, const GUID &wantSubtype)
{
    for (DWORD i = 0;; ++i) {
        IMFMediaType *type = nullptr;
        const HRESULT hr = input ? mft->GetInputAvailableType(0, i, &type)
                                 : mft->GetOutputAvailableType(0, i, &type);
        if (FAILED(hr) || type == nullptr)
            return nullptr;

        GUID subtype{};
        if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == wantSubtype)
            return type;
        type->Release();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct H264Decoder::Impl
{
    IMFTransform *mft = nullptr;

    int framesizeWidth  = 0;   // 解码器确认的实际宽度
    int framesizeHeight = 0;   // 解码器确认的实际高度
    int stride          = 0;   // NV12 输出每行字节数
    DWORD outputBufferSize = 0;
    bool  providesSamples  = false;
    bool  outputReady      = false;

    QString name;
    QString lastError;

    // 输出目标尺寸上限框（无效 = 不缩放，输出与码流等大）
    QSize outputSize;

    // 本解码器**所属线程**（首次 open() 时绑定的那个）。
    //
    // 为什么需要它：底层是 MF_ENUM_FLAG_SYNCMFT 的**同步软解 MFT**，这类 MFT 有
    // **线程亲和性** —— open()/ProcessInput/ProcessOutput/Release 全都必须在同一
    // 个线程上执行，跨线程搬运是未定义行为（典型症状是静默出错或 AV）。
    //
    // ⚠️ 这条约束靠「架构」保证（每个解码器都活在 DecoderWorker 的线程里），
    // 这个字段是**安全网**：一旦有人不小心跨线程用了，宁可给出可读错误也不要 UB。
    // 代价是每次调用一次 thread::id 比较，可忽略。
    std::thread::id owner;
    bool            bound = false;

    // 当前线程是否就是所属线程（未绑定时视为「可绑定」—— open() 里才真正绑定）
    bool onOwnerThread() const
    {
        return !bound || owner == std::this_thread::get_id();
    }

    // 跨线程误用时的统一报错
    bool rejectForeignThread(const char *what)
    {
        lastError = QStringLiteral("%1 不在解码器所属线程上（同步 MFT 有线程亲和性；"
                                   "应通过 DecoderWorker 使用）")
                        .arg(QLatin1String(what));
        return false;
    }

    void releaseTransform()
    {
        if (mft) {
            mft->Release();
            mft = nullptr;
        }
        outputReady = false;
    }

    // 解码器把真实画面尺寸放在输出类型的 MF_MT_FRAME_SIZE 上
    void applyOutputType(IMFMediaType *type)
    {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
            framesizeWidth  = static_cast<int>(w);
            framesizeHeight = static_cast<int>(h);
        }

        // NV12 的 Y 平面跨距；没给就用宽度（紧凑排布）
        UINT32 declared = 0;
        stride = framesizeWidth;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &declared))) {
            const auto signedStride = static_cast<qint32>(declared);
            if (signedStride >= framesizeWidth) {
                stride = signedStride;
            } else if (signedStride < 0) {
                // 负跨距 = bottom-up 排布。本路径按 top-down 取行，直接照用会得到上下翻转
                // 的画面 —— 显式记一笔，别让它**静默**变成错色/翻转。
                lastError = QStringLiteral("解码输出为负跨距（bottom-up，stride=%1），"
                                           "当前按紧凑排布解读，画面可能上下翻转")
                                .arg(signedStride);
            }
        }
    }

    // 首帧输入后解码器才确定输出类型，此时会返回 MF_E_TRANSFORM_STREAM_CHANGE
    bool renegotiateOutputType()
    {
        IMFMediaType *type = firstTypeMatching(mft, false, MFVideoFormat_NV12);
        if (type == nullptr)
            return false;

        const HRESULT hr = mft->SetOutputType(0, type, 0);
        if (SUCCEEDED(hr)) {
            applyOutputType(type);
            outputReady = true;

            // ⚠️ STREAM_CHANGE 后解码器可能已换尺寸 —— 输出样本参数必须**重查**。
            // 不重查就仍按 open() 时的旧值分配，样本永远偏小 ⇒ 每帧
            // MF_E_BUFFERTOOSMALL，而解码器不会被重建 ⇒ 画面永远好不了。
            MFT_OUTPUT_STREAM_INFO info{};
            if (SUCCEEDED(mft->GetOutputStreamInfo(0, &info))) {
                providesSamples  = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
                outputBufferSize = info.cbSize > 0 ? info.cbSize : outputBufferSize;
            }
        }
        type->Release();
        return SUCCEEDED(hr);
    }

    // 把一帧输出样本转成 QImage 追加到 frames。
    //
    // 这里的每一步失败都**只丢这一帧**（调用方仍算成功）—— 坏帧不该让整条链路判死。
    // 但绝不能**静默**：每种丢弃都写一条 lastError，否则「画面一直不更新」在两端
    // 都看不出任何线索（这正是 MF_E_BUFFERTOOSMALL 那类问题最难查的原因）。
    void emitSample(IMFSample *sample, QVector<QImage> &frames)
    {
        IMFMediaBuffer *contiguous = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&contiguous)) || contiguous == nullptr) {
            lastError = QStringLiteral("输出样本无法转成连续缓冲（ConvertToContiguousBuffer 失败），本帧丢弃");
            return;
        }

        BYTE *data   = nullptr;
        DWORD length = 0;
        // 「锁失败」和「锁上了却拿不到指针」是两件事，必须分开判：前者绝不能
        // Unlock（没锁上），后者必须 Unlock（已经锁上了）。挤进同一个条件里
        // 必然漏掉一边，缓冲的锁定计数就再也回不去。
        const HRESULT lockHr = contiguous->Lock(&data, nullptr, &length);
        if (FAILED(lockHr)) {
            lastError = QStringLiteral("输出缓冲 Lock 失败，本帧丢弃");
        } else if (data == nullptr) {
            lastError = QStringLiteral("输出缓冲 Lock 成功却没有数据指针，本帧丢弃");
            contiguous->Unlock();
        } else {
            const qint64 needed = static_cast<qint64>(stride) * framesizeHeight * 3 / 2;
            if (stride <= 0) {
                lastError = QStringLiteral("输出跨距非法（stride=%1），本帧丢弃").arg(stride);
            } else if (length < static_cast<DWORD>(needed)) {
                // 最可能的原因：STREAM_CHANGE 换了尺寸但输出样本仍按旧参数分配
                // （见 renegotiateOutputType 里的重查说明）。
                lastError = QStringLiteral("输出缓冲偏小（%1 < %2 字节，%3x%4 stride=%5），本帧丢弃")
                                .arg(length).arg(needed)
                                .arg(framesizeWidth).arg(framesizeHeight).arg(stride);
            } else {
                // 设了目标尺寸就「转换 + 等比缩小」一次做完 —— 4K 下这一步
                // 把 33 MB 的 ARGB32 写入降到格子量级，是单帧耗时的大头（实测 77%）。
                const QImage image =
                    outputSize.isValid()
                        ? nv12ToArgb32Scaled(data, stride, framesizeWidth, framesizeHeight,
                                             outputSize.width(), outputSize.height())
                        : nv12ToArgb32(data, stride, framesizeWidth, framesizeHeight);
                if (image.isNull()) {
                    lastError = QStringLiteral("NV12 -> ARGB32 转换失败（%1x%2 stride=%3%4），本帧丢弃")
                                    .arg(framesizeWidth).arg(framesizeHeight).arg(stride)
                                    .arg(outputSize.isValid()
                                             ? QStringLiteral(" 目标 %1x%2")
                                                   .arg(outputSize.width())
                                                   .arg(outputSize.height())
                                             : QString());
                } else {
                    frames.append(image);
                }
            }
            contiguous->Unlock();
        }
        contiguous->Release();
    }

    bool pullOutputs(QVector<QImage> &frames)
    {
        for (int guard = 0; guard < 64; ++guard) {
            MFT_OUTPUT_DATA_BUFFER buffer{};
            buffer.dwStreamID = 0;

            IMFSample *ours = nullptr;
            if (!providesSamples) {
                if (FAILED(::MFCreateSample(&ours)))
                    return false;
                IMFMediaBuffer *outBuffer = nullptr;
                if (FAILED(::MFCreateMemoryBuffer(outputBufferSize, &outBuffer))) {
                    ours->Release();
                    return false;
                }
                ours->AddBuffer(outBuffer);
                outBuffer->Release();
                buffer.pSample = ours;
            }

            DWORD   status = 0;
            HRESULT hr     = mft->ProcessOutput(0, 1, &buffer, &status);

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (ours)
                    ours->Release();
                if (buffer.pEvents)
                    buffer.pEvents->Release();
                return true;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (ours)
                    ours->Release();
                if (buffer.pEvents)
                    buffer.pEvents->Release();
                if (!renegotiateOutputType())
                    return false;
                continue;
            }
            if (FAILED(hr)) {
                lastError = hrText("ProcessOutput 失败", hr);
                if (ours)
                    ours->Release();
                if (buffer.pEvents)
                    buffer.pEvents->Release();
                return false;
            }

            if (buffer.pSample)
                emitSample(buffer.pSample, frames);
            if (buffer.pSample)
                buffer.pSample->Release();
            if (buffer.pEvents)
                buffer.pEvents->Release();
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

H264Decoder::H264Decoder() : m_impl(std::make_unique<Impl>()) {}

H264Decoder::~H264Decoder()
{
    close();
}

bool H264Decoder::isOpen() const
{
    return m_impl && m_impl->mft != nullptr;
}

int H264Decoder::width() const
{
    return m_impl ? m_impl->framesizeWidth : 0;
}

int H264Decoder::height() const
{
    return m_impl ? m_impl->framesizeHeight : 0;
}

void H264Decoder::setOutputSize(const QSize &size)
{
    if (m_impl)
        m_impl->outputSize = size;
}

QSize H264Decoder::outputSize() const
{
    return m_impl ? m_impl->outputSize : QSize();
}

QString H264Decoder::lastError() const
{
    return m_impl ? m_impl->lastError : QString();
}

QString H264Decoder::decoderName() const
{
    return m_impl ? m_impl->name : QString();
}

void H264Decoder::close()
{
    if (!m_impl)
        return;
    // 同步 MFT 的 Release/ProcessMessage 必须走在其所属线程上。
    // 跨线程调用时**拒绝且不改状态** —— 否则会把解码器置成「已关闭」，
    // 而所属线程那边还持着一个活的 MFT（泄漏 + 后续调用全乱）。
    if (!m_impl->onOwnerThread()) {
        m_impl->rejectForeignThread("close()");
        return;
    }
    if (m_impl->mft) {
        m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    m_impl->releaseTransform();
    m_impl->stride           = 0;
    m_impl->outputBufferSize = 0;
    m_impl->providesSamples  = false;
    // 解绑：close() 之后本实例可以换个线程重新 open()（DecoderWorker 重建路径就靠它）。
    m_impl->bound = false;
}

bool H264Decoder::open(int width, int height)
{
    // 先按「当前线程是否是所属线程」判一次 —— close() 此时可能被拒（不能真的关），
    // 所以要在 close() 之前把跨线程情况拦掉，别让它走到下面去。
    if (m_impl && m_impl->mft && !m_impl->onOwnerThread()) {
        m_impl->rejectForeignThread("open()");
        return false;
    }

    close();
    if (!m_impl)
        return false;
    m_impl->lastError.clear();

    if (width <= 0 || height <= 0) {
        m_impl->lastError = QStringLiteral("宽高必须为正");
        return false;
    }
    if (!ensureMfRuntime()) {
        m_impl->lastError = QStringLiteral("MFStartup 失败");
        return false;
    }

    // 绑定到**当前线程**。之后所有 MFT 操作都只允许在这个线程上做。
    m_impl->owner = std::this_thread::get_id();
    m_impl->bound = true;

    m_impl->framesizeWidth  = width;
    m_impl->framesizeHeight = height;

    // ---- 挑一个同步 H.264 解码器（优先微软的）----
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_NV12};

    IMFActivate **activates = nullptr;
    UINT32        count     = 0;
    HRESULT       hr        = ::MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                                          MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                          &inInfo, &outInfo, &activates, &count);
    if (FAILED(hr) || count == 0) {
        m_impl->lastError = hrText("找不到同步 H.264 解码器 MFT", hr);
        if (activates)
            ::CoTaskMemFree(activates);
        return false;
    }

    int chosen = -1;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR  friendly[512] = {0};
        UINT32 length        = 0;
        if (SUCCEEDED(activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, friendly, 511, &length))
            && std::wcsstr(friendly, L"H264 Video Decoder") != nullptr) {
            chosen = static_cast<int>(i);
            break;
        }
    }
    if (chosen < 0)
        chosen = 0;

    {
        WCHAR  friendly[512] = {0};
        UINT32 length        = 0;
        if (SUCCEEDED(activates[chosen]->GetString(MFT_FRIENDLY_NAME_Attribute, friendly, 511, &length)))
            m_impl->name = QString::fromWCharArray(friendly);
    }

    hr = activates[chosen]->ActivateObject(IID_PPV_ARGS(&m_impl->mft));
    for (UINT32 i = 0; i < count; ++i)
        activates[i]->Release();
    ::CoTaskMemFree(activates);
    if (FAILED(hr)) {
        m_impl->lastError = hrText("ActivateObject 失败", hr);
        m_impl->releaseTransform();
        return false;
    }

    // 远程桌面场景：尽量压低解码延迟
    if (IMFAttributes *attrs = nullptr; SUCCEEDED(m_impl->mft->GetAttributes(&attrs))) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->Release();
    }

    // ---- 输入类型固定为 H.264 ----
    IMFMediaType *inType = firstTypeMatching(m_impl->mft, true, MFVideoFormat_H264);
    if (inType == nullptr) {
        m_impl->lastError = QStringLiteral("解码器不接受 H.264 输入");
        m_impl->releaseTransform();
        return false;
    }
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, 30, 1);
    MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_impl->mft->SetInputType(0, inType, 0);
    inType->Release();
    if (FAILED(hr)) {
        m_impl->lastError = hrText("SetInputType(H264) 失败", hr);
        m_impl->releaseTransform();
        return false;
    }

    // ---- 输出类型：能现在定就现在定，否则等首帧的 STREAM_CHANGE ----
    if (IMFMediaType *outType = firstTypeMatching(m_impl->mft, false, MFVideoFormat_NV12)) {
        if (SUCCEEDED(m_impl->mft->SetOutputType(0, outType, 0))) {
            m_impl->applyOutputType(outType);
            m_impl->outputReady = true;
        }
        outType->Release();
    }

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    hr = m_impl->mft->GetOutputStreamInfo(0, &streamInfo);
    if (SUCCEEDED(hr)) {
        m_impl->providesSamples = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        m_impl->outputBufferSize = streamInfo.cbSize > 0
                                       ? streamInfo.cbSize
                                       : static_cast<DWORD>(width * height * 3 / 2 + 65536);
    } else {
        m_impl->outputBufferSize = static_cast<DWORD>(width * height * 3 / 2 + 65536);
    }

    m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Decoder::decodeFrame(const QByteArray &bitstream, QVector<QImage> &frames)
{
    if (!isOpen()) {
        m_impl->lastError = QStringLiteral("解码器未打开");
        return false;
    }
    // ProcessInput/ProcessOutput 必须与 open() 同线程（同步 MFT 的线程亲和性）
    if (!m_impl->onOwnerThread())
        return m_impl->rejectForeignThread("decodeFrame()");
    if (bitstream.isEmpty())
        return true;

    IMFMediaBuffer *buffer = nullptr;
    HRESULT         hr     = ::MFCreateMemoryBuffer(static_cast<DWORD>(bitstream.size()), &buffer);
    if (FAILED(hr)) {
        m_impl->lastError = hrText("MFCreateMemoryBuffer 失败", hr);
        return false;
    }

    BYTE *dest = nullptr;
    hr = buffer->Lock(&dest, nullptr, nullptr);
    if (FAILED(hr) || dest == nullptr) {
        buffer->Release();
        m_impl->lastError = hrText("输入缓冲 Lock 失败", hr);
        return false;
    }
    std::memcpy(dest, bitstream.constData(), static_cast<std::size_t>(bitstream.size()));
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bitstream.size()));

    IMFSample *sample = nullptr;
    hr = ::MFCreateSample(&sample);
    if (FAILED(hr)) {
        buffer->Release();
        m_impl->lastError = hrText("MFCreateSample 失败", hr);
        return false;
    }
    sample->AddBuffer(buffer);
    buffer->Release();

    hr = m_impl->mft->ProcessInput(0, sample, 0);
    sample->Release();

    if (hr == MF_E_NOTACCEPTING) {
        if (!m_impl->pullOutputs(frames))
            return false;
        // 重投一次
        IMFMediaBuffer *retryBuffer = nullptr;
        HRESULT retryHr = ::MFCreateMemoryBuffer(static_cast<DWORD>(bitstream.size()), &retryBuffer);
        if (FAILED(retryHr)) {
            // 提前失败也必须写 lastError：否则 decodeFailed 上报的是**上一次**的陈旧消息，
            // 排查时会被引到错误的方向。
            m_impl->lastError = hrText("重投建 buffer 失败", retryHr);
            return false;
        }
        BYTE *retryDest = nullptr;
        retryHr = retryBuffer->Lock(&retryDest, nullptr, nullptr);
        if (FAILED(retryHr) || retryDest == nullptr) {
            m_impl->lastError = hrText("重投 Lock 失败", FAILED(retryHr) ? retryHr : E_POINTER);
            retryBuffer->Release();
            return false;
        }
        std::memcpy(retryDest, bitstream.constData(), static_cast<std::size_t>(bitstream.size()));
        retryBuffer->Unlock();
        retryBuffer->SetCurrentLength(static_cast<DWORD>(bitstream.size()));

        IMFSample *retrySample = nullptr;
        retryHr = ::MFCreateSample(&retrySample);
        if (FAILED(retryHr)) {
            m_impl->lastError = hrText("重投建 sample 失败", retryHr);
            retryBuffer->Release();
            return false;
        }
        retrySample->AddBuffer(retryBuffer);
        retryBuffer->Release();
        hr = m_impl->mft->ProcessInput(0, retrySample, 0);
        retrySample->Release();
    }

    if (FAILED(hr)) {
        m_impl->lastError = hrText("ProcessInput 失败", hr);
        return false;
    }

    return m_impl->pullOutputs(frames);
}

bool H264Decoder::flush(QVector<QImage> &frames)
{
    if (!isOpen())
        return false;
    // 同 decodeFrame —— MFT 调用必须落在所属线程上
    if (!m_impl->onOwnerThread())
        return m_impl->rejectForeignThread("flush()");
    m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    return m_impl->pullOutputs(frames);
}

} // namespace lanrdp
