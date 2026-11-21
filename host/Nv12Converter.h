#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

// 前置声明：别把 d3d11.h 泄漏到调用方（与 ScreenCapturer.h 同一套路）
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11VideoDevice;
struct ID3D11VideoContext;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessorInputView;
struct ID3D11VideoProcessorOutputView;

namespace lanrdp {

// BGRA（DXGI 桌面复制的原生纹理）-> NV12（Media Foundation 编码器的输入）的加速器。
//
// 为什么要有这个类：4K 下 `capture + convert` 实测占掉一拍 40 ms 里的 25 ms
// （打点数据：capture 12~14 ms、convert 12.4 ms）。那 25 ms 里有 **两次 33 MB 的
// CPU 全量搬运**，而它们全是为了「先变成 QImage」这个中间形态：
//
//   旧：CopyResource → Map(BGRA) → memcpy 33 MB 进 QImage
//       → bgraToNv12 读 33 MB 写 12 MB
//   新：GPU 直接 VideoProcessorBlt 出 NV12 纹理 → Map 12 MB
//
// ⇒ 38 MB 的 CPU 流量 + 一次标量色彩转换整段消失，剩 12 MB 一次拷贝。
//
// **色彩必须与 `common/ColorConvert.h::bgraToNv12` 一致（BT.601 有限范围）**：
// 解码端 `nv12ToArgb32` 就是按 601 有限范围反变换的，这条链路一旦不同步，
// 现象是「画面整体发灰 / 偏色」，而且**不会报任何错**。驱动对 VideoProcessor 的
// 默认色彩空间各有各的默认值（很多是 BT.709），所以这里做两件事：
//   ① 显式下发输入（RGB 全范围）与输出（BT.601 有限范围）色彩空间；
//   ② **首次真正转换时**，把同一帧同时用 CPU 参考实现算一遍，逐样本比平均偏差；
//      超容差就整进程退回 CPU 并在 statusText() 里写清原因 —— 宁可慢 12 ms，
//      也不能让用户对着一张偏色的画面找不到原因。
class Nv12Converter
{
public:
    enum class Mode
    {
        Cpu,  // 只用 CPU（没有 D3D 设备 / 显卡自检没过 / 测试强制关闭）
        Gpu,  // ID3D11VideoProcessor
    };

    Nv12Converter();
    ~Nv12Converter();

    Nv12Converter(const Nv12Converter &)            = delete;
    Nv12Converter &operator=(const Nv12Converter &) = delete;

    // 绑定 D3D 设备（**只借用**：生命周期由 ScreenCapturer 负责，本类不 AddRef）。
    void bind(ID3D11Device *device, ID3D11DeviceContext *context);

    // 丢弃 GPU 侧全部对象（尺寸变化 / 抓屏器重建时调用）。mode_ 一并复位成 Gpu，
    // 但 statusText_ 保留 —— 那是一条「为什么退化」的诊断，不该因为重建就消失。
    void reset();

    Mode    mode() const { return mode_; }
    QString statusText() const { return statusText_; }

    // 只给测试：强制关掉 GPU 路径（不建处理器、不自检），验证降级链路。
    void setGpuAllowed(bool on);

    // 从 BGRA 纹理直接产出**紧凑** NV12（Y 平面 width*height + 交织 UV）。
    // ⚠️ 调用方必须仍在持有该帧（尚未 `ReleaseFrame`）时调用。
    // 返回 false 时 error 给原因，out 不被修改；此时调用方应退回 CPU 路径
    //（本类若已自行降级，mode() 会变成 Cpu）。
    bool convertTexture(ID3D11Texture2D *bgra, int width, int height, QByteArray *out,
                        QString *error = nullptr);

    // 纯 CPU 参考实现：读任意 BGRA 内存（stride 可大于 width*4），输出紧凑 NV12。
    // 与 `common/ColorConvert.h::bgraToNv12` 逐字节一致（同一套整数系数）。
    static bool convertCpu(const uchar *src, int strideBytes, int width, int height,
                           QByteArray *out);

    // 两份紧凑 NV12 的平均绝对偏差（按 luma / chroma 分别给，0~255 量纲）。
    // 抽成静态纯函数是为了让「容差判定」能脱机单测 —— 它决定要不要降级。
    // 采样步长 step（>=1），大图上只比对 1/step² 的样本，够判色彩空间错没错。
    struct Deviation
    {
        double luma   = 0.0;
        double chroma = 0.0;
        bool   valid  = false;
    };
    static Deviation meanDeviation(const QByteArray &a, const QByteArray &b, int width,
                                   int height, int step = 4);

    // 判定容差：BT.601 / BT.709 / 范围（16-235 与 0-255）任一处错都会远大于它。
    // 真机实测的驱动细节差（色度采样位置、舍入）在 1~2 之内。
    static constexpr double kMaxLumaDeviation   = 3.0;
    static constexpr double kMaxChromaDeviation = 6.0;

private:
    bool ensureGpu(int width, int height, QString *error);
    bool verifyAgainstCpu(ID3D11Texture2D *bgra, int width, int height, QByteArray *out,
                          QString *error);
    bool gpuConvert(ID3D11Texture2D *bgra, int width, int height, QByteArray *out,
                    QString *error);
    void releaseGpu();
    void degradeToCpu(const QString &reason);

    ID3D11Device                   *device_  = nullptr;  // 借用
    ID3D11DeviceContext            *context_ = nullptr;  // 借用
    ID3D11VideoDevice              *videoDevice_  = nullptr;
    ID3D11VideoContext             *videoContext_ = nullptr;
    ID3D11VideoProcessorEnumerator *enumerator_   = nullptr;
    ID3D11VideoProcessor           *processor_    = nullptr;
    ID3D11VideoProcessorInputView  *inputView_    = nullptr;
    // 输入视图是**按源纹理**建的。DXGI 桌面复制通常每帧复用同一张纹理，
    // 所以这里缓存「当前视图对应的那张」，换了才重建（不缓存就是每帧一次分配）。
    ID3D11Texture2D                *inputViewTexture_ = nullptr;
    ID3D11VideoProcessorOutputView *outputView_   = nullptr;
    ID3D11Texture2D                *nv12Texture_  = nullptr;  // 渲染目标（GPU 写）
    ID3D11Texture2D                *nv12Staging_  = nullptr;  // CPU 可读

    // 自检用的临时 BGRA 暂存（只在第一次转换时建，比完即放）
    ID3D11Texture2D *probeStaging_ = nullptr;

    int         gpuWidth_   = 0;
    int         gpuHeight_  = 0;
    bool        gpuAllowed_ = true;
    bool        verified_   = false;  // 自检是否已做过（**失败也置 true**，只做一次）
    Mode        mode_       = Mode::Cpu;   // 未 bind 前一律报 CPU（如实回答，别假装有显卡）
    QString     statusText_;
};

} // namespace lanrdp
