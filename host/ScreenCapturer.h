#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

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

// 一块可抓的显示输出（多屏切换的最小信息集）。
//
// `index` 是**枚举序**，不是 DXGI 的 DeviceName：插拔 / 改缩放后同一块屏的 index
// 可能变，所以线上（`SCREEN_LIST`）每次都带完整几何，消费方按「index + 几何」理解。
// ⚠️ 枚举与抓屏**必须共用同一段遍历**（实现里的 `collectOutputs`），
// 否则「列表第 2 项」与「抓第 2 项」会指向不同的屏 —— 那种错位不会报错，只会选错屏。
struct MonitorInfo
{
    int     index     = -1;
    QRect   rect;             // 物理像素，屏幕坐标系（主屏左侧 / 上方时为负）
    bool    isPrimary = false;
    QString name;             // 诊断用（"\\\\.\\DISPLAY1"），不参与任何判定
};

// 基于 DXGI Desktop Duplication 的桌面抓屏（默认主显示器，可指定输出）。
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
    // `preferredOutputIndex` 为 `enumerateOutputs()` 里的 index；传 -1（默认）= 沿用
    // 「主显示器优先，退化到第一个已附着的输出」。传了越界值（那块屏被拔掉 / 枚举顺序
    // 变了）时**回落主屏并照常成功** —— 切屏失败不该让整条链路黑屏，实际选中的 index
    // 由 `outputIndex()` 报出去，调用方据此校正界面。
    //
    // 失败分两类，靠 recoverable 区分（传 nullptr 表示不关心）：
    // · recoverable=true  —— 「现在不行、等会儿可能行」：`DuplicateOutput` 返
    //   E_ACCESSDENIED（被控端**启动时**正锁屏 / 被远控软件独占显示输出）。
    //   调用方应**限频重试**而不是放弃（与运行中 ACCESS_LOST 的
    //   RecoverableLost 语义一致）。
    // · recoverable=false —— 「真错」：显卡 / 输出不可用、设备创建失败等。
    bool init(QString *error = nullptr, bool *recoverable = nullptr,
              int preferredOutputIndex = -1);
    void shutdown();

    // 当前所有**可抓**的显示输出，顺序即 index。
    //
    // 只收「非软件适配器 + AttachedToDesktop」的输出：休眠 / 被禁用 / 虚拟显示器的
    // 空输出不进列表，否则界面上会多出一项点开就黑屏的「屏 3」。
    // 静态方法：枚举本身不需要 D3D 设备，控制端菜单刷新时随时可调。
    static QVector<MonitorInfo> enumerateOutputs();

    // init() 实际选中的输出 index；未初始化成功时为 -1。
    // ⚠️ 报的是**真实**选中的那个，不是调用方请求的那个（见 init 的回落语义）。
    int outputIndex() const { return outputIndex_; }

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
    int                     outputIndex_ = -1;   // 实际选中的枚举 index（见 outputIndex()）
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
