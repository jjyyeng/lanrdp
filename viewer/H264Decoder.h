#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

#include <memory>

namespace lanrdp {

// Media Foundation H.264 解码器封装（同步 MFT，微软 "Microsoft H264 Video Decoder MFT"）。
//
// 输入 Annex-B 码流（关键帧需自带 SPS/PPS），输出 QImage(Format_ARGB32)。
//
// ⚠️ **线程约束**：底层是 `MFT_ENUM_FLAG_SYNCMFT` 的**纯软解**，同步 MFT
// 有**线程亲和性** —— 实例首次 `open()` 的那个线程就是它的归属线程，
// `decodeFrame()` / `flush()` / `close()` 都必须在该线程上调用。
// 跨线程调用会被拒绝并写入 `lastError()`（不崩、不静默出错）。
//
// 正常用法是**不要直接用本类**：把它交给 `viewer/DecoderWorker`（专属解码线程），
// `DecoderWorker` 会在自己的线程里创建/打开/使用/销毁它。
class H264Decoder
{
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder &)            = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;

    // width/height 只是提示值；解码器解析出码流头后会以实际尺寸为准（见 width()/height()）。
    bool open(int width, int height);
    void close();
    bool isOpen() const;

    // 送入一帧 Annex-B 码流；可能解出 0..N 张画面（解码器有重排序延迟）。
    bool decodeFrame(const QByteArray &bitstream, QVector<QImage> &frames);

    // 冲刷流水线里残余的帧。
    bool flush(QVector<QImage> &frames);

    // 解码器确定的实际画面尺寸（尚未解出首帧时返回 open() 传入的提示值）
    int width() const;
    int height() const;

    // 输出画面的**目标尺寸上限框**（等比、永不放大、box 平均缩小）。
    //
    // 传 `QSize()`（默认，-1×-1）即关闭缩放，输出与码流等大。
    // ⚠️ **不影响 width()/height()** —— 那两个始终报告**码流真实尺寸**，
    // 状态栏分辨率、归一化坐标映射都依赖这个口径（缩放只是显示侧省带宽的手段）。
    void  setOutputSize(const QSize &size);
    QSize outputSize() const;

    // **最近一次可报告的问题**（`open()` 会清空）。
    //
    // ⚠️ 语义不是「本次调用失败」：有两种情况都会写它 ——
    // ① 返回 false 的致命失败；② 只丢了一帧、调用方仍算成功的情况（输出缓冲偏小、
    //    负跨距、颜色转换失败……）。后者不写就会让「画面一直不更新」完全无线索。
    // 所以判断「是否致命」要看返回值，别只看它是否为空。
    QString lastError() const;
    QString decoderName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lanrdp
