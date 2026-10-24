#include "host/ScreenCapturer.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>

namespace lanrdp {

namespace {

void setError(QString *error, const QString &text, HRESULT hr = S_OK)
{
    if (!error)
        return;
    if (hr == S_OK) {
        *error = text;
    } else {
        *error = QStringLiteral("%1 (HRESULT=0x%2)")
                     .arg(text)
                     .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
    }
}

const char *hrName(HRESULT hr)
{
    switch (hr) {
    case DXGI_ERROR_UNSUPPORTED:              return "DXGI_ERROR_UNSUPPORTED";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:  return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
    case DXGI_ERROR_ACCESS_LOST:              return "DXGI_ERROR_ACCESS_LOST";
    case DXGI_ERROR_DEVICE_REMOVED:           return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET:             return "DXGI_ERROR_DEVICE_RESET";
    case E_ACCESSDENIED:                      return "E_ACCESSDENIED";
    default:                                  return "unexpected error";
    }
}

// 这些错误表示「复制对象或设备已经没了」，重建是唯一出路。
bool isRecoverableLoss(HRESULT hr)
{
    return hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED
           || hr == DXGI_ERROR_DEVICE_RESET;
}

// 格式名（只为把错误写成「人话」，让用户知道该关什么）。
const char *formatName(unsigned int fmt)
{
    switch (fmt) {
    case static_cast<unsigned int>(DXGI_FORMAT_B8G8R8A8_UNORM):     return "B8G8R8A8_UNORM";
    case static_cast<unsigned int>(DXGI_FORMAT_R10G10B10A2_UNORM):  return "R10G10B10A2_UNORM";
    case static_cast<unsigned int>(DXGI_FORMAT_R16G16B16A16_FLOAT): return "R16G16B16A16_FLOAT";
    default:                                                       return "未知格式";
    }
}

// 不支持时统一给这条：说清「是什么格式」+「怎么办」。HDR 是唯一现实成因
// （桌面复制会给出该显示器的当前模式格式）。
QString unsupportedFormatMessage(unsigned int fmt)
{
    return QStringLiteral("不支持的桌面格式 %1：请在 Windows 显示设置里关闭 HDR 后重试")
        .arg(QString::fromLatin1(formatName(fmt)));
}

} // namespace

bool ScreenCapturer::isSupportedFormat(unsigned int dxgiFormat)
{
    return dxgiFormat == static_cast<unsigned int>(DXGI_FORMAT_B8G8R8A8_UNORM);
}

ScreenCapturer::ScreenCapturer() = default;

ScreenCapturer::~ScreenCapturer()
{
    shutdown();
}

bool ScreenCapturer::init(QString *error, bool *recoverable)
{
    shutdown();
    // 默认「真错」；只有明确属于「等会儿可能行」的分支才置 true。
    if (recoverable)
        *recoverable = false;

    // ---- 1. DXGI 工厂 ----
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void **>(&factory));
    if (FAILED(hr)) {
        setError(error, QStringLiteral("CreateDXGIFactory1 失败"), hr);
        return false;
    }

    // ---- 2. 选输出：优先匹配“主显示器”所在的输出 ----
    // 机器上常存在虚拟显示适配器（ToDesk / Honor / Microsoft Remote Display 等），
    // 盲取第一个输出可能抓到虚拟屏或黑屏，因此以 Win32 的主显示器坐标为准。
    RECT primary{};
    {
        POINT      origin{0, 0};
        HMONITOR   monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(MONITORINFO);
        if (monitor && GetMonitorInfoW(monitor, &mi))
            primary = mi.rcMonitor;
    }

    IDXGIAdapter1 *adapter = nullptr;
    IDXGIOutput   *output  = nullptr;
    QString        adapterName;
    QString        outputName;
    RECT           outputRect{};

    auto findOutput = [&](bool wantPrimary) -> bool {
        for (UINT ai = 0;; ++ai) {
            IDXGIAdapter1 *candidate = nullptr;
            // 只与 DXGI_ERROR_NOT_FOUND 比较是不够的：其它失败（如 E_ACCESSDENIED）
            // 会把 candidate 留成 nullptr，紧接着的 candidate->GetDesc1 就是空指针解引用。
            const HRESULT ah = factory->EnumAdapters1(ai, &candidate);
            if (FAILED(ah) || candidate == nullptr)
                break;

            DXGI_ADAPTER_DESC1 adapterDesc{};
            if (FAILED(candidate->GetDesc1(&adapterDesc))) {
                candidate->Release();
                continue;   // 这个适配器读不出描述（驱动异常），跳过而不是崩
            }
            const bool isSoftware = (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

            bool matched = false;
            if (!isSoftware) {
                for (UINT oi = 0;; ++oi) {
                    IDXGIOutput *candidateOutput = nullptr;
                    // 同上：非 NOT_FOUND 的失败也会留下空指针。
                    const HRESULT oh = candidate->EnumOutputs(oi, &candidateOutput);
                    if (FAILED(oh) || candidateOutput == nullptr)
                        break;

                    DXGI_OUTPUT_DESC outputDesc{};
                    if (FAILED(candidateOutput->GetDesc(&outputDesc))) {
                        candidateOutput->Release();
                        continue;   // 读不出描述的输出，跳过
                    }

                    const RECT &r = outputDesc.DesktopCoordinates;
                    const bool primaryMatch =
                        outputDesc.AttachedToDesktop && primary.right > primary.left
                        && r.left == primary.left && r.top == primary.top
                        && r.right == primary.right && r.bottom == primary.bottom;

                    const bool accept = wantPrimary ? primaryMatch : outputDesc.AttachedToDesktop;
                    if (accept) {
                        adapter     = candidate;
                        output      = candidateOutput;
                        adapterName = QString::fromWCharArray(adapterDesc.Description);
                        outputName  = QString::fromWCharArray(outputDesc.DeviceName);
                        outputRect  = r;
                        matched     = true;
                        break;  // 保留引用，不 Release
                    }
                    candidateOutput->Release();
                }
            }

            if (matched)
                return true;
            candidate->Release();
        }
        return false;
    };

    if (!findOutput(true))
        findOutput(false);  // 找不到主显示器匹配时，退化为第一个已附着桌面的输出

    factory->Release();

    if (!adapter || !output) {
        setError(error, QStringLiteral("未找到可用的显示输出（显卡/显示器不可用）"));
        if (adapter) adapter->Release();
        return false;
    }

    deviceInfo_ = QStringLiteral("%1 | %2 [%3,%4 %5x%6]")
                      .arg(adapterName,
                           outputName)
                      .arg(outputRect.left)
                      .arg(outputRect.top)
                      .arg(static_cast<int>(outputRect.right - outputRect.left))
                      .arg(static_cast<int>(outputRect.bottom - outputRect.top));

    // ---- 3. D3D11 设备（必须建在拥有该输出的适配器上）----
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL        got = {};
    hr = D3D11CreateDevice(adapter,
                           D3D_DRIVER_TYPE_UNKNOWN,
                           nullptr,
                           0,
                           levels,
                           static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                           D3D11_SDK_VERSION,
                           &device_,
                           &got,
                           &context_);
    adapter->Release();
    if (FAILED(hr)) {
        output->Release();
        setError(error, QStringLiteral("D3D11CreateDevice 失败"), hr);
        shutdown();
        return false;
    }

    // ---- 4. 桌面复制器 ----
    IDXGIOutput1 *output1 = nullptr;
    hr = output->QueryInterface(IID_IDXGIOutput1, reinterpret_cast<void **>(&output1));
    output->Release();
    if (FAILED(hr)) {
        setError(error, QStringLiteral("获取 IDXGIOutput1 失败（系统需 Windows 8+）"), hr);
        shutdown();
        return false;
    }

    hr = output1->DuplicateOutput(device_, &duplication_);
    output1->Release();
    if (FAILED(hr)) {
        setError(error,
                 QStringLiteral("DuplicateOutput 失败：%1（锁屏 / 无登录会话 / 已被其他程序占用）")
                     .arg(QString::fromLatin1(hrName(hr))),
                 hr);
        shutdown();
        // 「现在不行、等会儿可能行」：启动时正锁屏、或显示输出被 ToDesk / 向日葵
        // 等远控软件独占。这类失败与运行中 ACCESS_LOST 同源 —— 桌面回来后能好，
        // 所以标记为可恢复，让调用方限频重试而不是彻底停流。
        // ⚠️ 必须是白名单：别的错误（尤其 DXGI_ERROR_UNSUPPORTED —— 适配器压根不支持
        // 桌面复制）重试多少次都是同一个结果，标成可恢复只会让调用方在重建里空转。
        if (recoverable
            && (hr == E_ACCESSDENIED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
                || hr == DXGI_ERROR_SESSION_DISCONNECTED))
            *recoverable = true;
        return false;
    }

    DXGI_OUTDUPL_DESC dupDesc{};
    duplication_->GetDesc(&dupDesc);

    // 格式白名单。下游按「每像素 4 字节 BGRA」解读画面，别的格式必须**在这里就报错** ——
    // 开 HDR 后 ModeDesc.Format 会是 R10G10B10A2_UNORM（错色）甚至
    // R16G16B16A16_FLOAT（行距翻倍 ⇒ 错行），而全程每一步都「成功」，日志干净，
    // 最后只表现为「画面颜色不对」这种最难定位的故障。
    if (!isSupportedFormat(static_cast<unsigned int>(dupDesc.ModeDesc.Format))) {
        setError(error, unsupportedFormatMessage(static_cast<unsigned int>(dupDesc.ModeDesc.Format)));
        shutdown();
        return false;
    }

    width_  = static_cast<int>(dupDesc.ModeDesc.Width);
    height_ = static_cast<int>(dupDesc.ModeDesc.Height);

    // 记录被抓显示器在屏幕坐标系中的位置：注入端据此把归一化坐标还原到正确的屏上。
    monitorRect_ = QRect(static_cast<int>(outputRect.left),
                         static_cast<int>(outputRect.top),
                         width_,
                         height_);

    ready_  = true;
    return true;
}

void ScreenCapturer::shutdown()
{
    if (staging_) {
        staging_->Release();
        staging_ = nullptr;
    }
    stagingW_ = stagingH_ = 0;

    if (duplication_) {
        duplication_->Release();
        duplication_ = nullptr;
    }
    if (context_) {
        context_->Release();
        context_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    ready_  = false;
    width_  = 0;
    height_ = 0;
    monitorRect_ = QRect();
}

CaptureStatus ScreenCapturer::capture(QImage *out, int timeoutMs, QString *error)
{
    if (!out)
        return CaptureStatus::Failed;

    if (!ready_ || !duplication_) {
        setError(error, QStringLiteral("抓屏器未初始化"));
        return CaptureStatus::Failed;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource         *resource = nullptr;
    HRESULT                hr = duplication_->AcquireNextFrame(static_cast<UINT>(timeoutMs), &info, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return CaptureStatus::NoNewFrame;

    // 复制对象 / 设备失效 —— **先自我作废，再报状态**。
    // · 旧实现在这里只 setError 就 return，ready_ 仍是 true、duplication_ 还是那个死指针
    // ⇒ 调用方（ScreenStreamer）既不会重建，capturedRect() 还会一直返回**过期几何**，
    // 键鼠注入持续打向旧显示器。
    // · shutdown() 会把 ready_ 置 false 并清掉 monitorRect_，让 isReady()/monitorRect()
    // 对外说实话；重建由调用方负责（init() 本身先 shutdown()，重试是安全的）。
    if (isRecoverableLoss(hr)) {
        setError(error,
                 QStringLiteral("桌面复制已失效（会话切换 / 分辨率变化 / 设备重置）：%1，需重新初始化")
                     .arg(QString::fromLatin1(hrName(hr))),
                 hr);
        shutdown();
        return CaptureStatus::RecoverableLost;
    }

    if (FAILED(hr)) {
        setError(error, QStringLiteral("AcquireNextFrame 失败"), hr);
        return CaptureStatus::Failed;
    }

    ID3D11Texture2D *texture = nullptr;
    hr = resource->QueryInterface(IID_ID3D11Texture2D, reinterpret_cast<void **>(&texture));
    resource->Release();
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        setError(error, QStringLiteral("拿到的资源不是 ID3D11Texture2D"), hr);
        return CaptureStatus::Failed;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    // 逐帧再查一次：会话中途打开 HDR 时 ModeDesc 会在**不重建复制器**的情况下变，
    // 只查 init 那次会漏掉。
    if (!isSupportedFormat(static_cast<unsigned int>(desc.Format))) {
        texture->Release();
        duplication_->ReleaseFrame();
        setError(error, unsupportedFormatMessage(static_cast<unsigned int>(desc.Format)));
        return CaptureStatus::Failed;
    }

    // 暂存纹理（CPU 可读），尺寸变化时重建
    if (!staging_ || stagingW_ != static_cast<int>(desc.Width)
        || stagingH_ != static_cast<int>(desc.Height)) {
        if (staging_) {
            staging_->Release();
            staging_ = nullptr;
        }
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.BindFlags      = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags      = 0;
        sd.MipLevels      = 1;
        sd.ArraySize      = 1;
        hr = device_->CreateTexture2D(&sd, nullptr, &staging_);
        if (FAILED(hr)) {
            texture->Release();
            duplication_->ReleaseFrame();
            setError(error, QStringLiteral("创建暂存纹理失败"), hr);
            return CaptureStatus::Failed;
        }
        stagingW_ = static_cast<int>(desc.Width);
        stagingH_ = static_cast<int>(desc.Height);
    }

    // GPU 拷贝到暂存纹理，随后立即释放帧
    context_->CopyResource(staging_, texture);
    texture->Release();
    duplication_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        setError(error, QStringLiteral("映射暂存纹理失败"), hr);
        return CaptureStatus::Failed;
    }

    const int w        = static_cast<int>(desc.Width);
    const int h        = static_cast<int>(desc.Height);
    const int rowBytes = w * 4;

    QImage image(w, h, QImage::Format_ARGB32);
    // 构造失败（内存压力 / 尺寸被 Qt 拒绝）时 image 为 null，scanLine() 返回空指针 ⇒ memcpy 崩。
    // 这里必须**先 Unmap 再返回**，否则暂存纹理一直映射着，后续 Map 会失败。
    if (image.isNull()) {
        context_->Unmap(staging_, 0);
        setError(error, QStringLiteral("分配画面缓冲失败（内存不足）"));
        return CaptureStatus::Failed;
    }

    const auto *src = static_cast<const uchar *>(mapped.pData);
    for (int y = 0; y < h; ++y) {
        std::memcpy(image.scanLine(y), src + static_cast<qsizetype>(y) * mapped.RowPitch, rowBytes);
    }

    context_->Unmap(staging_, 0);

    *out = image;
    return CaptureStatus::Ok;
}

} // namespace lanrdp
