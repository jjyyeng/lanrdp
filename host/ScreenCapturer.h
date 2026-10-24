#pragma once

#include <QImage>
#include <QRect>
#include <QString>

// 前置声明，避免把 d3d11/dxgi 头文件泄漏到调用方
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIOutputDuplication;

namespace lanrdp {

// 抓屏状态
enum class CaptureStatus
{
    Ok,          // 抓到一帧新画面
    NoNewFrame,  // 桌面无变化（超时），调用方可跳过本次发送

    // 桌面复制（或 D3D 设备）已失效 —— ACCESS_LOST / DEVICE_REMOVED /
    // DEVICE_RESET。UAC 安全桌面、锁屏解锁、Ctrl+Alt+Del、分辨率变化、显示器唤醒都会命中。
    //
    // 语义与 Failed 的关键差别：**重建后通常能恢复**，所以调用方应当重新 init（或重建本对象）
    // 并重试，而不是放弃。返回本状态时对象已**自我作废**（shutdown 过，isReady()==false，
    // monitorRect() 为空）—— 这样调用方不会拿着过期几何继续注入坐标。
    RecoverableLost,

    Failed,      // 其他错误（配置 / 资源类，详见 error）
};

// 基于 DXGI Desktop Duplication 的桌面抓屏（主显示器）。
//
// 注意：Desktop Duplication 只能抓取“已登录的活动桌面会话”，
// 锁屏 / 无登录会话 / 服务态下会失败，这是 Windows 的限制。
class ScreenCapturer
{
public:
    ScreenCapturer();
    ~ScreenCapturer();

    ScreenCapturer(const ScreenCapturer &)            = delete;
    ScreenCapturer &operator=(const ScreenCapturer &) = delete;

    // 初始化 D3D11 设备与桌面复制器。失败时 error 给出原因。
    //
    // 失败分两类，靠 recoverable 区分（传 nullptr 表示不关心）：
    // · recoverable=true  —— 「现在不行、等会儿可能行」：`DuplicateOutput` 返
    //   E_ACCESSDENIED（被控端**启动时**正锁屏 / 被远控软件独占显示输出）。
    //   调用方应**限频重试**而不是放弃（与运行中 ACCESS_LOST 的
    //   RecoverableLost 语义一致）。
    // · recoverable=false —— 「真错」：显卡 / 输出不可用、设备创建失败等。
    bool init(QString *error = nullptr, bool *recoverable = nullptr);
    void shutdown();

    // 桌面复制拿到的纹理格式必须是 B8G8R8A8_UNORM —— 下游按「每像素 4 字节 BGRA」
    // 解读画面（`rowBytes = width * 4`、`QImage::Format_ARGB32`）。别的格式
    // （开 HDR 后常见 R10G10B10A2_UNORM / R16G16B16A16_FLOAT）会错色、甚至
    // 因行距翻倍而错行，且全程不报错。抽成静态纯函数是为了让这条判断能脱离
    // 真实 DXGI 环境单测。
    static bool isSupportedFormat(unsigned int dxgiFormat);

    bool isReady() const { return ready_; }
    int  width() const { return width_; }
    int  height() const { return height_; }

    // 被抓显示器在屏幕坐标系中的矩形（多显示器时的关键信息：
    // 控制端送来的归一化坐标要靠它还原到正确的屏幕上）。
    QRect monitorRect() const { return monitorRect_; }

    // 实际选中的显卡/输出描述（诊断用，init 后有效）
    QString deviceInfo() const { return deviceInfo_; }

    // 抓一帧。输出的 QImage 为 Format_ARGB32（内存布局 BGRA，与 DXGI 一致）。
    // 桌面格式不在支持范围（见 isSupportedFormat）时返回 Failed 并给出可行动的错误，
    // 而不是照着错色画下去。
    CaptureStatus capture(QImage *out, int timeoutMs = 100, QString *error = nullptr);

private:
    bool                    ready_     = false;
    int                     width_     = 0;
    int                     height_    = 0;
    QRect                   monitorRect_;
    QString                 deviceInfo_;
    ID3D11Device           *device_    = nullptr;
    ID3D11DeviceContext    *context_   = nullptr;
    IDXGIOutputDuplication *duplication_ = nullptr;
    ID3D11Texture2D        *staging_   = nullptr;
    int                     stagingW_  = 0;
    int                     stagingH_  = 0;
};

} // namespace lanrdp
