#include "host/Nv12Converter.h"

#include "common/ColorConvert.h"

#include <d3d11.h>

#include <cmath>
#include <cstring>

namespace lanrdp {

namespace {

void setError(QString *error, const QString &text, HRESULT hr)
{
    if (!error)
        return;
    if (hr == S_OK)
        *error = text;
    else
        *error = QStringLiteral("%1（HRESULT=0x%2）")
                     .arg(text)
                     .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

// 紧凑 NV12 的字节数：Y 平面 + 交织 UV 平面（色度向上取整，与 bgraToNv12 同口径）。
qsizetype nv12Bytes(int width, int height)
{
    const qsizetype cw = (width + 1) / 2;
    const qsizetype ch = (height + 1) / 2;
    return static_cast<qsizetype>(width) * height + cw * ch * 2;
}

} // namespace

Nv12Converter::Nv12Converter() = default;

Nv12Converter::~Nv12Converter()
{
    releaseGpu();
}

void Nv12Converter::bind(ID3D11Device *device, ID3D11DeviceContext *context)
{
    if (device_ == device && context_ == context)
        return;

    // 设备一换，旧对象全部失效（它们都是那个设备的子对象）。
    releaseGpu();
    device_  = device;
    context_ = context;

    // 换了设备 ⇒ 之前的自检结论不再作数，得在新设备上重来一次。
    verified_ = false;
    mode_     = (gpuAllowed_ && device_ && context_) ? Mode::Gpu : Mode::Cpu;
}

void Nv12Converter::setGpuAllowed(bool on)
{
    if (gpuAllowed_ == on)
        return;

    gpuAllowed_ = on;
    if (!on) {
        releaseGpu();
        mode_ = Mode::Cpu;
        // 措辞要与 `degradeToCpu` 那句区分开：那是「驱动/色彩不行」，这是「用户关的」。
        // 现场排查时这两者要采取的动作完全不同，混成一句就再也分不清了。
        statusText_ = QStringLiteral("显卡色彩转换已被手动关闭，改用 CPU 直读");
    } else {
        mode_     = (device_ && context_) ? Mode::Gpu : Mode::Cpu;
        verified_ = false;   // 重开必须重验色彩，不能拿上一次的结论顶账
        statusText_ = QStringLiteral("显卡色彩转换已重新启用，等待下一帧校验");
    }
}

void Nv12Converter::reset()
{
    releaseGpu();
    gpuWidth_  = 0;
    gpuHeight_ = 0;
    verified_  = false;
    mode_      = (gpuAllowed_ && device_ && context_) ? Mode::Gpu : Mode::Cpu;
}

void Nv12Converter::releaseGpu()
{
    // 释放顺序无关（COM 引用计数），但保持「视图 → 纹理 → 处理器」的可读性。
    auto drop = [](auto *&p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    };
    drop(inputView_);
    inputViewTexture_ = nullptr;
    drop(outputView_);
    drop(nv12Staging_);
    drop(nv12Texture_);
    drop(probeStaging_);
    drop(processor_);
    drop(enumerator_);
    drop(videoContext_);
    drop(videoDevice_);
}

void Nv12Converter::degradeToCpu(const QString &reason)
{
    mode_ = Mode::Cpu;
    statusText_ = QStringLiteral("显卡色彩转换已停用，改用 CPU 直读：%1").arg(reason);
    releaseGpu();
}

// ---------------------------------------------------------------------------
// CPU 参考实现（= bgraToNv12，一批不改）
// ---------------------------------------------------------------------------

bool Nv12Converter::convertCpu(const uchar *src, int strideBytes, int width, int height,
                               QByteArray *out)
{
    if (!out || !src || width <= 0 || height <= 0)
        return false;
    if (strideBytes < width * 4)
        return false;

    QByteArray nv12 = bgraToNv12(src, strideBytes, width, height);
    if (nv12.isEmpty())
        return false;
    *out = std::move(nv12);
    return true;
}

Nv12Converter::Deviation Nv12Converter::meanDeviation(const QByteArray &a, const QByteArray &b,
                                                      int width, int height, int step)
{
    Deviation d;
    if (width <= 0 || height <= 0 || step < 1)
        return d;

    const qsizetype need = nv12Bytes(width, height);
    if (a.size() < need || b.size() < need)
        return d;

    const auto *pa = reinterpret_cast<const uchar *>(a.constData());
    const auto *pb = reinterpret_cast<const uchar *>(b.constData());

    double sumLuma = 0.0;
    qint64 lumaSamples = 0;
    for (int y = 0; y < height; y += step) {
        const uchar *ra = pa + static_cast<qsizetype>(y) * width;
        const uchar *rb = pb + static_cast<qsizetype>(y) * width;
        for (int x = 0; x < width; x += step) {
            sumLuma += std::abs(static_cast<int>(ra[x]) - static_cast<int>(rb[x]));
            ++lumaSamples;
        }
    }

    const int     chromaWidth = (width + 1) / 2;
    const int     chromaRows  = (height + 1) / 2;
    const qsizetype uvBase    = static_cast<qsizetype>(width) * height;
    const int     uvRowBytes  = chromaWidth * 2;

    double sumChroma = 0.0;
    qint64 chromaSamples = 0;
    for (int y = 0; y < chromaRows; y += step) {
        const uchar *ra = pa + uvBase + static_cast<qsizetype>(y) * uvRowBytes;
        const uchar *rb = pb + uvBase + static_cast<qsizetype>(y) * uvRowBytes;
        for (int x = 0; x < uvRowBytes; x += step) {
            sumChroma += std::abs(static_cast<int>(ra[x]) - static_cast<int>(rb[x]));
            ++chromaSamples;
        }
    }

    if (lumaSamples <= 0 || chromaSamples <= 0)
        return d;

    d.luma   = sumLuma / static_cast<double>(lumaSamples);
    d.chroma = sumChroma / static_cast<double>(chromaSamples);
    d.valid  = true;
    return d;
}

// ---------------------------------------------------------------------------
// GPU 路径
// ---------------------------------------------------------------------------

bool Nv12Converter::ensureGpu(int width, int height, QString *error)
{
    if (!gpuAllowed_) {
        mode_ = Mode::Cpu;
        return false;
    }
    if (mode_ == Mode::Cpu)
        return false;
    if (!device_ || !context_) {
        setError(error, QStringLiteral("未绑定 D3D 设备"), S_OK);
        mode_ = Mode::Cpu;
        return false;
    }
    // NV12 在 D3D 里要求偶数宽高（解码端也只接受偶数，见 ScreenStreamer 的检查）。
    if ((width % 2) != 0 || (height % 2) != 0) {
        setError(error, QStringLiteral("尺寸 %1x%2 为奇数，NV12 不支持").arg(width).arg(height),
                 S_OK);
        return false;
    }
    if (processor_ && gpuWidth_ == width && gpuHeight_ == height)
        return true;

    // 尺寸变了：处理器 / 输出纹理都绑在这个尺寸上，必须整体重建。
    releaseGpu();

    HRESULT hr = S_OK;
    if (!videoDevice_) {
        hr = device_->QueryInterface(IID_ID3D11VideoDevice,
                                     reinterpret_cast<void **>(&videoDevice_));
        if (FAILED(hr)) {
            setError(error, QStringLiteral("显卡不支持 ID3D11VideoDevice"), hr);
            mode_ = Mode::Cpu;
            return false;
        }
    }
    if (!videoContext_) {
        hr = context_->QueryInterface(IID_ID3D11VideoContext,
                                      reinterpret_cast<void **>(&videoContext_));
        if (FAILED(hr)) {
            setError(error, QStringLiteral("显卡不支持 ID3D11VideoContext"), hr);
            mode_ = Mode::Cpu;
            return false;
        }
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate.Numerator   = 60;
    content.InputFrameRate.Denominator = 1;
    content.InputWidth   = static_cast<UINT>(width);
    content.InputHeight  = static_cast<UINT>(height);
    content.OutputWidth  = static_cast<UINT>(width);
    content.OutputHeight = static_cast<UINT>(height);
    content.Usage        = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = videoDevice_->CreateVideoProcessorEnumerator(&content, &enumerator_);
    if (FAILED(hr)) {
        setError(error, QStringLiteral("创建视频处理器枚举器失败"), hr);
        mode_ = Mode::Cpu;
        return false;
    }

    UINT outSupport = 0;
    hr = enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outSupport);
    if (FAILED(hr) || (outSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        setError(error, QStringLiteral("显卡不支持把 NV12 作为视频处理器输出"), hr);
        mode_ = Mode::Cpu;
        return false;
    }

    hr = videoDevice_->CreateVideoProcessor(enumerator_, 0, &processor_);
    if (FAILED(hr)) {
        setError(error, QStringLiteral("创建视频处理器失败"), hr);
        mode_ = Mode::Cpu;
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width              = static_cast<UINT>(width);
    td.Height             = static_cast<UINT>(height);
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_NV12;
    td.SampleDesc.Count   = 1;
    td.SampleDesc.Quality = 0;
    td.Usage              = D3D11_USAGE_DEFAULT;
    td.BindFlags          = D3D11_BIND_RENDER_TARGET;
    td.CPUAccessFlags     = 0;

    hr = device_->CreateTexture2D(&td, nullptr, &nv12Texture_);
    if (FAILED(hr)) {
        setError(error, QStringLiteral("创建 NV12 渲染纹理失败"), hr);
        mode_ = Mode::Cpu;
        return false;
    }

    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device_->CreateTexture2D(&sd, nullptr, &nv12Staging_);
    if (FAILED(hr)) {
        setError(error, QStringLiteral("创建 NV12 暂存纹理失败"), hr);
        mode_ = Mode::Cpu;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension         = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice    = 0;
    hr = videoDevice_->CreateVideoProcessorOutputView(nv12Texture_, enumerator_, &ovd,
                                                      &outputView_);
    if (FAILED(hr)) {
        setError(error, QStringLiteral("创建视频处理器输出视图失败"), hr);
        mode_ = Mode::Cpu;
        return false;
    }

    // ---- 色彩空间：必须与 bgraToNv12 / nv12ToArgb32 的 BT.601 有限范围对齐 ----
    // 输入是桌面合成结果（BGRA 全范围 0-255）；输出是给编码器吃的 NV12。
    // 不显式下发的话，各家驱动的默认值不同（很多默认 BT.709）⇒ 画面发灰/偏色，
    // 而且全程零报错。下面的自检是这条设置的最后一道保险。
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inSpace{};
    inSpace.Usage         = 0;   // video playback
    inSpace.RGB_Range     = 0;   // 0 = 全范围 0-255
    inSpace.YCbCr_Matrix  = 0;   // BT.601（RGB 输入时无意义，保持同口径）
    inSpace.YCbCr_xvYCC   = 0;
    inSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_, 0, &inSpace);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outSpace{};
    outSpace.Usage         = 0;
    outSpace.RGB_Range     = 0;
    outSpace.YCbCr_Matrix  = 0;   // BT.601 —— 与 ColorConvert 的整数系数一致
    outSpace.YCbCr_xvYCC   = 0;
    outSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetOutputColorSpace(processor_, &outSpace);

    videoContext_->VideoProcessorSetStreamFrameFormat(processor_, 0,
                                                      D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    gpuWidth_  = width;
    gpuHeight_ = height;
    return true;
}

bool Nv12Converter::gpuConvert(ID3D11Texture2D *bgra, int width, int height, QByteArray *out,
                               QString *error)
{
    if (!bgra || !out)
        return false;

    HRESULT hr = S_OK;

    // 输入视图绑在源纹理上：DXGI 复用同一张纹理时这条分支只走一次。
    if (!inputView_ || inputViewTexture_ != bgra) {
        if (inputView_) {
            inputView_->Release();
            inputView_ = nullptr;
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.FourCC                         = 0;
        ivd.ViewDimension                  = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice             = 0;
        ivd.Texture2D.ArraySlice           = 0;
        hr = videoDevice_->CreateVideoProcessorInputView(bgra, enumerator_, &ivd, &inputView_);
        if (FAILED(hr)) {
            setError(error, QStringLiteral("创建视频处理器输入视图失败"), hr);
            return false;
        }
        inputViewTexture_ = bgra;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable            = TRUE;
    stream.OutputIndex       = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames        = 0;
    stream.FutureFrames      = 0;
    stream.pInputSurface     = inputView_;

    hr = videoContext_->VideoProcessorBlt(processor_, outputView_, 0, 1, &stream);
    if (FAILED(hr)) {
        setError(error, QStringLiteral("显卡色彩转换（VideoProcessorBlt）失败"), hr);
        return false;
    }

    context_->CopyResource(nv12Staging_, nv12Texture_);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(nv12Staging_, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        setError(error, QStringLiteral("映射 NV12 暂存纹理失败"), hr);
        return false;
    }

    const int     pitch = static_cast<int>(mapped.RowPitch);
    const uchar  *src   = static_cast<const uchar *>(mapped.pData);
    const qsizetype yBytes = static_cast<qsizetype>(width) * height;

    QByteArray nv12(nv12Bytes(width, height), Qt::Uninitialized);
    if (nv12.size() != nv12Bytes(width, height)) {
        context_->Unmap(nv12Staging_, 0);
        setError(error, QStringLiteral("分配 NV12 缓冲失败（内存不足）"), S_OK);
        return false;
    }

    char *dst = nv12.data();
    if (pitch == width) {
        // 紧凑布局：Y 与 UV 首尾相接、行距相同 ⇒ 一次全量拷贝（12 MB）
        std::memcpy(dst, src, static_cast<size_t>(yBytes + yBytes / 2));
    } else {
        for (int y = 0; y < height; ++y) {
            std::memcpy(dst + static_cast<qsizetype>(y) * width,
                        src + static_cast<qsizetype>(y) * pitch, static_cast<size_t>(width));
        }
        // DXGI 的 NV12 布局：UV 平面紧跟在 Y 平面之后，起始行为 pitch * height
        const uchar *uvSrc = src + static_cast<qsizetype>(pitch) * height;
        for (int y = 0; y < height / 2; ++y) {
            std::memcpy(dst + yBytes + static_cast<qsizetype>(y) * width,
                        uvSrc + static_cast<qsizetype>(y) * pitch,
                        static_cast<size_t>(width));
        }
    }

    context_->Unmap(nv12Staging_, 0);

    *out = std::move(nv12);
    return true;
}

// 首次转换的「出厂检验」：同一帧同时用 CPU 参考实现算一遍，比平均偏差。
// 只在第一次走，之后不再付这个代价（判过一次就够，驱动不会中途改主意）。
bool Nv12Converter::verifyAgainstCpu(ID3D11Texture2D *bgra, int width, int height,
                                     QByteArray *out, QString *error)
{
    QByteArray gpuOut;
    if (!gpuConvert(bgra, width, height, &gpuOut, error)) {
        // GPU 这条路当场就瘸了：直接退 CPU，让调用方本帧改走直读路径。
        degradeToCpu(QStringLiteral("首次转换失败"));
        return false;
    }

    // ---- CPU 参考：同一张 BGRA 纹理拷到可读暂存，再按老系数转 ----
    QByteArray reference;
    {
        if (!probeStaging_) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width              = static_cast<UINT>(width);
            sd.Height             = static_cast<UINT>(height);
            sd.MipLevels          = 1;
            sd.ArraySize          = 1;
            sd.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count   = 1;
            sd.Usage              = D3D11_USAGE_STAGING;
            sd.BindFlags          = 0;
            sd.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&sd, nullptr, &probeStaging_))) {
                probeStaging_ = nullptr;
            }
        }

        if (probeStaging_) {
            context_->CopyResource(probeStaging_, bgra);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context_->Map(probeStaging_, 0, D3D11_MAP_READ, 0, &mapped))) {
                convertCpu(static_cast<const uchar *>(mapped.pData),
                           static_cast<int>(mapped.RowPitch), width, height, &reference);
                context_->Unmap(probeStaging_, 0);
            }
            // 一次性用途：比完就放，别为了一行诊断常驻 33 MB 显存
            probeStaging_->Release();
            probeStaging_ = nullptr;
        }
    }

    if (reference.isEmpty()) {
        // 参考都算不出来，就没法确认色彩对不对。保守：本帧用 GPU 结果（总比丢帧强），
        // 但从此不再走 GPU —— 宁可慢，也不要让偏色无人知晓。
        degradeToCpu(QStringLiteral("无法生成 CPU 参考帧"));
        *out = std::move(gpuOut);
        return true;
    }

    const Deviation d = meanDeviation(gpuOut, reference, width, height);
    if (!d.valid || d.luma > kMaxLumaDeviation || d.chroma > kMaxChromaDeviation) {
        // 偏差超容差 = 驱动的色彩空间没按我们的要求来。这一帧用可信的 CPU 版本，
        // 下一帧起走 CPU 直读（不再有 QImage 中间态，仍比旧路径快）。
        degradeToCpu(QStringLiteral("显卡色彩与 CPU 参考不一致：亮度偏差 %1、色度偏差 %2")
                         .arg(d.luma, 0, 'f', 1)
                         .arg(d.chroma, 0, 'f', 1));
        *out = std::move(reference);
        return true;
    }

    statusText_ = QStringLiteral("显卡色彩转换已启用（与 CPU 参考偏差：亮度 %1、色度 %2）")
                      .arg(d.luma, 0, 'f', 1)
                      .arg(d.chroma, 0, 'f', 1);
    *out = std::move(gpuOut);
    return true;
}

bool Nv12Converter::convertTexture(ID3D11Texture2D *bgra, int width, int height, QByteArray *out,
                                   QString *error)
{
    if (error)
        error->clear();
    if (!bgra || !out || width <= 0 || height <= 0)
        return false;

    if (!ensureGpu(width, height, error))
        return false;

    if (!verified_) {
        verified_ = true;   // 只做一次；失败也置位，避免每帧重试
        return verifyAgainstCpu(bgra, width, height, out, error);
    }
    return gpuConvert(bgra, width, height, out, error);
}

} // namespace lanrdp
