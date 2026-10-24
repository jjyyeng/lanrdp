#include "host/H264Encoder.h"

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

#include <cwchar>
#include <cstring>
#include <mutex>

namespace lanrdp {

namespace {

int be32At(const QByteArray &data, int offset)
{
    return (static_cast<quint8>(data.at(offset)) << 24)
           | (static_cast<quint8>(data.at(offset + 1)) << 16)
           | (static_cast<quint8>(data.at(offset + 2)) << 8)
           | static_cast<quint8>(data.at(offset + 3));
}

bool hasStartCodeAt(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 3 > data.size())
        return false;
    if (static_cast<quint8>(data.at(offset)) != 0 || static_cast<quint8>(data.at(offset + 1)) != 0)
        return false;
    if (static_cast<quint8>(data.at(offset + 2)) == 1)
        return true;
    return offset + 4 <= data.size() && static_cast<quint8>(data.at(offset + 2)) == 0
           && static_cast<quint8>(data.at(offset + 3)) == 1;
}

// 起始码占用 3 还是 4 字节
int startCodeLengthAt(const QByteArray &data, int offset)
{
    return static_cast<quint8>(data.at(offset + 2)) == 1 ? 3 : 4;
}

// 把 SPS(7) / PPS(8) 抽出来，转成带 4 字节起始码的 Annex-B 片段。
// 微软编码器只在**第一个**关键帧里带一次参数集，后续关键帧不再重复，
// 因此必须自己缓存下来，给每个关键帧补齐——否则中途加入的观看者无法解码。
QByteArray extractParameterSets(const QByteArray &data)
{
    QByteArray out;
    const int  size = data.size();

    int offset = 0;
    while (offset + 3 <= size) {
        if (!hasStartCodeAt(data, offset)) {
            ++offset;
            continue;
        }
        const int headerOffset = offset + startCodeLengthAt(data, offset);
        if (headerOffset >= size)
            break;

        const int nalType = static_cast<quint8>(data.at(headerOffset)) & 0x1F;

        int next = headerOffset;
        while (next < size && !hasStartCodeAt(data, next))
            ++next;

        if (nalType == 7 || nalType == 8) {
            out.append("\x00\x00\x00\x01", 4);
            out.append(data.constData() + headerOffset, next - headerOffset);
        }
        offset = next > headerOffset ? next : headerOffset + 1;
    }
    return out;
}

bool containsNalType(const QByteArray &data, int nalType)
{
    for (int i = 0; i + 4 < data.size(); ++i) {
        if (hasStartCodeAt(data, i)) {
            const int headerOffset = i + (static_cast<quint8>(data.at(i + 2)) == 1 ? 3 : 4);
            if (headerOffset < data.size()
                && (static_cast<quint8>(data.at(headerOffset)) & 0x1F) == nalType)
                return true;
        }
    }
    return false;
}

IMFMediaType *pickAvailableType(IMFTransform *mft, bool input, const GUID &wantSubtype)
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

IMFMediaType *findAvailableOutputType(IMFTransform *mft, const GUID &wantSubtype)
{
    return pickAvailableType(mft, false, wantSubtype);
}

// MinGW 没有为 ICodecAPI 提供 __mingw_uuidof 特化，无法用 IID_PPV_ARGS，只能显式给 GUID
const GUID kIID_ICodecAPI =
    {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

bool setVariantU32(ICodecAPI *api, const GUID &key, quint32 value)
{
    if (api == nullptr)
        return false;
    VARIANT var;
    VariantInit(&var);
    var.vt     = VT_UI4;
    var.ulVal  = value;
    const bool ok = SUCCEEDED(api->SetValue(&key, &var));
    VariantClear(&var);
    return ok;
}

} // namespace

// 编码器输出可能是 Annex-B（带起始码），也可能是 AVCC（4 字节大端长度前缀）。
// 统一成 Annex-B，方便下游按起始码扫描 NALU。已经是 Annex-B 或解析失败时原样返回。
QByteArray normalizeToAnnexB(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    if (hasStartCodeAt(data, 0))
        return data;

    QByteArray out;
    out.reserve(data.size() + 16);
    int offset = 0;
    while (offset + 4 <= data.size()) {
        const int nalSize = be32At(data, offset);
        offset += 4;
        // 用减法比较而不是 `offset + nalSize > data.size()`：nalSize 来自线上数据
        // （最大 0x7FFFFFFF），加 offset 会**有符号溢出成负数**，把这条边界检查整个绕过。
        if (nalSize <= 0 || nalSize > data.size() - offset) {
            return data;   // 解析失败就原样返回，别把数据弄坏
        }
        out.append("\x00\x00\x00\x01", 4);
        out.append(data.constData() + offset, nalSize);
        offset += nalSize;
    }
    return out.isEmpty() ? data : out;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct H264Encoder::Impl
{
    IMFTransform *mft      = nullptr;
    ICodecAPI    *codecApi = nullptr;

    int width           = 0;
    int height          = 0;
    int fps             = 0;
    int bitrate         = 0;
    int expectedNv12    = 0;
    LONGLONG frameIndex = 0;

    DWORD outputBufferSize = 0;
    bool  providesSamples  = false;
    // ICodecAPI 是可选接口：拿不到时码率与强制关键帧都下发不了（见 open()）。
    bool  bitrateControlAvailable = false;

    QByteArray parameterSets;   // SPS/PPS（Annex-B）
    QString    name;
    QString    lastError;

    void releaseTransform()
    {
        if (codecApi) { codecApi->Release(); codecApi = nullptr; }
        if (mft)      { mft->Release();      mft      = nullptr; }
        bitrateControlAvailable = false;
    }

    // 把最近一次取到的输出类型上的 SPS/PPS 记下来
    void refreshParameterSets(IMFMediaType *type)
    {
        if (type == nullptr)
            return;
        UINT32 blobSize = 0;
        if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blobSize)) || blobSize == 0)
            return;
        QByteArray blob(static_cast<int>(blobSize), '\0');
        if (SUCCEEDED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                    reinterpret_cast<UINT8 *>(blob.data()), blobSize, &blobSize)))
            parameterSets = normalizeToAnnexB(blob.left(static_cast<int>(blobSize)));
    }

    void emitSample(IMFSample *sample, QVector<H264EncodedFrame> &outputs)
    {
        UINT32 cleanPoint = 0;
        const bool mfCleanPoint =
            SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint))
            && cleanPoint != 0;

        IMFMediaBuffer *contiguous = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&contiguous)) || contiguous == nullptr)
            return;

        BYTE *data = nullptr;
        DWORD length = 0;
        // Lock 的成败必须和「拿到的指针能不能用」分开判：Lock 失败时缓冲根本没有
        // 被锁住，再调一次 Unlock 违反 IMFMediaBuffer 的 Lock/Unlock 配对契约。
        const HRESULT lockHr = contiguous->Lock(&data, nullptr, &length);
        if (SUCCEEDED(lockHr) && data != nullptr && length > 0) {
            QByteArray raw(reinterpret_cast<const char *>(data), static_cast<int>(length));
            contiguous->Unlock();

            H264EncodedFrame frame;
            frame.bitstream = normalizeToAnnexB(raw);

            // 关键帧判定以**码流里有没有 IDR(NAL 5)** 为准：
            // MF 的 MFSampleExtension_CleanPoint 只在第一个 IDR 上置位，后续 IDR 不带，
            // 光靠它会让中途加入的观看者永远等不到「关键帧」标记。
            frame.keyframe = mfCleanPoint || containsNalType(frame.bitstream, 5);

            // 先学习：哪一帧带了完整参数集就缓存下来（通常是第一个关键帧）
            if (containsNalType(frame.bitstream, 7) && containsNalType(frame.bitstream, 8)) {
                const QByteArray sets = extractParameterSets(frame.bitstream);
                if (!sets.isEmpty())
                    parameterSets = sets;
            }

            // 再补齐：关键帧必须自包含，否则中途加入的观看者拿不到参数集
            if (frame.keyframe && !parameterSets.isEmpty()
                && !containsNalType(frame.bitstream, 7)) {
                frame.bitstream.prepend(parameterSets);
            }

            if (!frame.bitstream.isEmpty())
                outputs.append(frame);
        } else if (SUCCEEDED(lockHr)) {
            contiguous->Unlock();   // 锁住了、只是内容为空：这一份必须还回去
        }
        contiguous->Release();
    }

    // 输出类型可能中途变化（编码器在首帧后确定最终参数），此时需重设并刷新参数集
    bool renegotiateOutputType()
    {
        IMFMediaType *type = findAvailableOutputType(mft, MFVideoFormat_H264);
        if (type == nullptr)
            return false;
        type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1);
        const HRESULT hr = mft->SetOutputType(0, type, 0);
        if (SUCCEEDED(hr))
            refreshParameterSets(type);
        type->Release();
        return SUCCEEDED(hr);
    }

    bool pullOutputs(QVector<H264EncodedFrame> &outputs)
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
                emitSample(buffer.pSample, outputs);
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

H264Encoder::H264Encoder() : m_impl(std::make_unique<Impl>()) {}

H264Encoder::~H264Encoder()
{
    close();
}

bool H264Encoder::isOpen() const
{
    return m_impl && m_impl->mft != nullptr;
}

int H264Encoder::width() const
{
    return m_impl ? m_impl->width : 0;
}

int H264Encoder::height() const
{
    return m_impl ? m_impl->height : 0;
}

QString H264Encoder::lastError() const
{
    return m_impl ? m_impl->lastError : QString();
}

QString H264Encoder::encoderName() const
{
    return m_impl ? m_impl->name : QString();
}

void H264Encoder::close()
{
    if (!m_impl)
        return;
    if (m_impl->mft) {
        m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    m_impl->releaseTransform();
    m_impl->parameterSets.clear();
    m_impl->frameIndex      = 0;
    m_impl->expectedNv12    = 0;
    m_impl->providesSamples = false;
    m_impl->outputBufferSize = 0;
}

bool H264Encoder::open(int width, int height, int fps, int bitrateBps, int gopSeconds)
{
    close();
    if (!m_impl)
        return false;
    m_impl->lastError.clear();

    if (width <= 0 || height <= 0 || fps <= 0 || bitrateBps <= 0) {
        m_impl->lastError = QStringLiteral("参数非法：width/height/fps/bitrate 必须为正");
        return false;
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        m_impl->lastError = QStringLiteral("NV12 要求宽高为偶数，收到 %1x%2").arg(width).arg(height);
        return false;
    }
    if (!ensureMfRuntime()) {
        m_impl->lastError = QStringLiteral("MFStartup 失败");
        return false;
    }

    // ---- 挑一个同步编码器（优先微软的 H264 Encoder MFT）----
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};

    IMFActivate **activates = nullptr;
    UINT32        count     = 0;
    HRESULT       hr        = ::MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                          MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                          &inInfo, &outInfo, &activates, &count);
    if (FAILED(hr) || count == 0) {
        m_impl->lastError = hrText("找不到同步 H.264 编码器 MFT", hr);
        if (activates)
            ::CoTaskMemFree(activates);
        return false;
    }

    int chosen = -1;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR  friendly[512] = {0};
        UINT32 length        = 0;
        if (SUCCEEDED(activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, friendly, 511, &length))
            && std::wcsstr(friendly, L"H264 Encoder MFT") != nullptr) {
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

    // ---- 降低流水线延迟（远程桌面场景很关键）----
    if (IMFAttributes *attrs = nullptr; SUCCEEDED(m_impl->mft->GetAttributes(&attrs))) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->Release();
    }

    // ---- 先配输出类型，编码器据此推导输入类型 ----
    IMFMediaType *outType = findAvailableOutputType(m_impl->mft, MFVideoFormat_H264);
    if (outType == nullptr) {
        m_impl->lastError = QStringLiteral("编码器没有 H.264 输出类型");
        m_impl->releaseTransform();
        return false;
    }
    outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrateBps));
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (gopSeconds > 0)
        outType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(fps * gopSeconds));

    hr = m_impl->mft->SetOutputType(0, outType, 0);
    if (FAILED(hr)) {
        m_impl->lastError = hrText("SetOutputType(H264) 失败", hr);
        outType->Release();
        m_impl->releaseTransform();
        return false;
    }
    outType->Release();

    // ---- 再配输入类型 ----
    IMFMediaType *inType = pickAvailableType(m_impl->mft, true, MFVideoFormat_NV12);
    if (inType == nullptr) {
        m_impl->lastError = QStringLiteral("编码器不接受 NV12 输入");
        m_impl->releaseTransform();
        return false;
    }
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_impl->mft->SetInputType(0, inType, 0);
    if (FAILED(hr)) {
        m_impl->lastError = hrText("SetInputType(NV12) 失败", hr);
        inType->Release();
        m_impl->releaseTransform();
        return false;
    }
    inType->Release();

    // ---- 码率控制 / GOP（尽力而为，失败不影响可用性）----
    // ⚠️ 实测（bench_adaptive_bitrate）：open 阶段设的码率**不生效**——微软同步
    // H.264 编码器在收到首个输入帧前会忽略一切码率设置，open 传 8M / 1M / 200k
    // 输出都在默认 ~7.6–9.5 Mbps。真正生效的是「首帧之后」的 setBitrate()。
    // 这里保留 open 时的设置仅作兼容（对某些编码器或未来场景可能有用），
    // 上层 ScreenStreamer 必须在首帧编码后调一次 setBitrate 设真实码率。
    if (SUCCEEDED(m_impl->mft->QueryInterface(kIID_ICodecAPI,
                                               reinterpret_cast<void **>(&m_impl->codecApi)))) {
        m_impl->bitrateControlAvailable = true;
        setVariantU32(m_impl->codecApi, CODECAPI_AVEncCommonRateControlMode,
                      eAVEncCommonRateControlMode_CBR);
        setVariantU32(m_impl->codecApi, CODECAPI_AVEncCommonMeanBitRate,
                      static_cast<quint32>(bitrateBps));
        if (gopSeconds > 0)
            setVariantU32(m_impl->codecApi, CODECAPI_AVEncMPVGOPSize,
                          static_cast<quint32>(fps * gopSeconds));
    } else {
        // ICodecAPI 是**可选**接口。拿不到不代表 open() 要失败（还能编码、只是
        // 参数不可控），但绝不能像从前那样静默 —— 记下标志，让上层能问出来并上报。
        m_impl->bitrateControlAvailable = false;
    }

    // ---- 记录 SPS/PPS，并问清楚输出缓冲要多大 ----
    if (IMFMediaType *actual = nullptr; SUCCEEDED(m_impl->mft->GetOutputCurrentType(0, &actual))) {
        m_impl->refreshParameterSets(actual);
        actual->Release();
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

    m_impl->width        = width;
    m_impl->height       = height;
    m_impl->fps          = fps;
    m_impl->bitrate      = bitrateBps;
    m_impl->expectedNv12 = width * height * 3 / 2;
    m_impl->frameIndex   = 0;

    m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_impl->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    return true;
}

bool H264Encoder::setBitrate(int bitrateBps)
{
    if (!isOpen() || !m_impl)
        return false;
    if (bitrateBps <= 0) {
        m_impl->lastError = QStringLiteral("bitrate 必须为正");
        return false;
    }
    if (m_impl->codecApi == nullptr) {
        m_impl->lastError = QStringLiteral("编码器未提供 ICodecAPI，无法动态改码率");
        return false;
    }
    const bool ok = setVariantU32(m_impl->codecApi, CODECAPI_AVEncCommonMeanBitRate,
                                  static_cast<quint32>(bitrateBps));
    if (ok)
        m_impl->bitrate = bitrateBps;
    else
        m_impl->lastError = QStringLiteral("编码器拒绝了新的平均码率 %1 bit/s").arg(bitrateBps);
    return ok;
}

bool H264Encoder::bitrateControlAvailable() const
{
    return m_impl && m_impl->bitrateControlAvailable;
}

bool H264Encoder::encodeFrame(const uchar *nv12, int byteCount, bool forceKeyframe,
                              QVector<H264EncodedFrame> &outputs)
{
    if (!isOpen()) {
        m_impl->lastError = QStringLiteral("编码器未打开");
        return false;
    }
    if (nv12 == nullptr || byteCount != m_impl->expectedNv12) {
        m_impl->lastError = QStringLiteral("NV12 数据长度不匹配：期望 %1，收到 %2")
                                .arg(m_impl->expectedNv12)
                                .arg(byteCount);
        return false;
    }

    if (forceKeyframe) {
        if (m_impl->codecApi == nullptr) {
            // 不是致命错（画面照出），但必须留痕：新观看者拿不到 IDR 就只能
            // 干等下一个自然关键帧（GOP 长度，可能好几秒才有画面）。
            m_impl->lastError = QStringLiteral("编码器未提供 ICodecAPI，无法强制关键帧"
                                               "（新观看者可能要等到下一个 GOP 才有画面）");
        } else if (!setVariantU32(m_impl->codecApi, CODECAPI_AVEncVideoForceKeyFrame, 1)) {
            m_impl->lastError = QStringLiteral("编码器拒绝了强制关键帧请求");
        }
    }

    IMFMediaBuffer *buffer = nullptr;
    HRESULT         hr     = ::MFCreateMemoryBuffer(static_cast<DWORD>(byteCount), &buffer);
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
    std::memcpy(dest, nv12, static_cast<std::size_t>(byteCount));
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(byteCount));

    IMFSample *sample = nullptr;
    hr = ::MFCreateSample(&sample);
    if (FAILED(hr)) {
        buffer->Release();
        m_impl->lastError = hrText("MFCreateSample 失败", hr);
        return false;
    }
    sample->AddBuffer(buffer);
    buffer->Release();

    const LONGLONG duration = 10000000LL / m_impl->fps;
    sample->SetSampleTime(m_impl->frameIndex * duration);
    sample->SetSampleDuration(duration);

    hr = m_impl->mft->ProcessInput(0, sample, 0);
    sample->Release();

    // 编码器内部队列满了：先把已完成的输出取走，再重投这一帧
    if (hr == MF_E_NOTACCEPTING) {
        if (!m_impl->pullOutputs(outputs))
            return false;

        IMFMediaBuffer *retryBuffer = nullptr;
        if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(byteCount), &retryBuffer)))
            return false;
        BYTE *retryDest = nullptr;
        if (FAILED(retryBuffer->Lock(&retryDest, nullptr, nullptr)) || retryDest == nullptr) {
            retryBuffer->Release();
            return false;
        }
        std::memcpy(retryDest, nv12, static_cast<std::size_t>(byteCount));
        retryBuffer->Unlock();
        retryBuffer->SetCurrentLength(static_cast<DWORD>(byteCount));

        IMFSample *retrySample = nullptr;
        if (FAILED(::MFCreateSample(&retrySample))) {
            retryBuffer->Release();
            return false;
        }
        retrySample->AddBuffer(retryBuffer);
        retryBuffer->Release();
        retrySample->SetSampleTime(m_impl->frameIndex * duration);
        retrySample->SetSampleDuration(duration);

        hr = m_impl->mft->ProcessInput(0, retrySample, 0);
        retrySample->Release();
    }

    if (FAILED(hr)) {
        m_impl->lastError = hrText("ProcessInput 失败", hr);
        return false;
    }

    ++m_impl->frameIndex;
    return m_impl->pullOutputs(outputs);
}

bool H264Encoder::flush(QVector<H264EncodedFrame> &outputs)
{
    if (!isOpen())
        return false;
    m_impl->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    return m_impl->pullOutputs(outputs);
}

} // namespace lanrdp
