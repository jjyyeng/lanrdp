#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <memory>

namespace lanrdp {

// 编码器产出的一帧
struct H264EncodedFrame
{
    QByteArray bitstream;   // Annex-B；若该帧是关键帧，SPS/PPS 已前置
    bool       keyframe = false;
};

// 把 AVCC（每个 NALU 前置 4 字节大端长度）统一成 Annex-B（起始码分隔）。
// 已经是 Annex-B 或解析失败时原样返回。独立成公开函数便于单测。
QByteArray normalizeToAnnexB(const QByteArray &data);

// Media Foundation H.264 编码器封装。
//
// 输入 NV12，输出 Annex-B。宽高必须为偶数（NV12 的色度平面要求）。
//
// 目前只使用**同步** MFT（MF_TRANSFORM_ASYNC = 0，即微软的 "H264 Encoder MFT"）。
// 本机可用的硬件编码器（Intel Quick Sync / NVIDIA NVENC）全是异步 MFT，必须实现
// METransformNeedInput / METransformHaveOutput 事件循环，本项目暂未实现。
class H264Encoder
{
public:
    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder &)            = delete;
    H264Encoder &operator=(const H264Encoder &) = delete;

    // bitrateBps 为平均码率；gopSeconds 决定关键帧间隔（<=0 表示用编码器默认值）。
    bool open(int width, int height, int fps, int bitrateBps, int gopSeconds = 2);
    void close();
    bool isOpen() const;

    // 运行中动态改平均码率（不重建编码器、不改尺寸/帧率/GOP）。
    // 返回 false 表示编码器未打开、参数非法，或没取到 ICodecAPI。
    // ⚠️ 是否即时生效取决于编码器实现（部分同步 MFT 要等下个 GOP 边界或 FLUSH），
    // 调用方不应假定立刻降码。可行性见 bench_adaptive_bitrate 实测。
    bool setBitrate(int bitrateBps);

    // 编码器是否提供了 `ICodecAPI`（**可选**接口，不是所有同步 MFT 都有）。
    // 为 false 时：setBitrate() 必然失败（返回 false，lastError 说明原因）；
    // encodeFrame(forceKeyframe=true) 没法强制 IDR，新观看者只能等下一个自然关键帧。
    // 单独暴露这个标志，是要让上层**一次**判断并上报，而不是每帧静默重试。
    bool bitrateControlAvailable() const;

    // open() 时确定的尺寸
    int width() const;
    int height() const;

    // 送入一帧 NV12；byteCount 必须等于 width * height * 3 / 2。
    // 编码器有内部流水线延迟，可能本次不产出任何帧（outputs 保持为空）。
    bool encodeFrame(const uchar *nv12, int byteCount, bool forceKeyframe,
                     QVector<H264EncodedFrame> &outputs);

    // 冲刷流水线里残余的帧。
    bool flush(QVector<H264EncodedFrame> &outputs);

    QString lastError() const;
    QString encoderName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lanrdp
