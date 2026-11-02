#pragma once

#include <QByteArray>
#include <QImage>
#include <QtGlobal>

namespace lanrdp {

// BGRA（DXGI 桌面复制的原生格式）-> NV12（Media Foundation 编码器要求的输入格式）。
//
// NV12 内存布局 = Y 平面（width * height 字节）+ 交织 UV 平面
// （chromaWidth * chromaHeight * 2 字节，顺序 U,V,U,V...）
// 其中 chromaWidth = ceil(width/2)、chromaHeight = ceil(height/2)。
//
// 色彩上采用 BT.601 有限范围（video range）：Y ∈ [16,235]、Cb/Cr ∈ [16,240]，
// 色度由 2x2 像素块取平均后计算。宽高为奇数时，缺省像素按边缘复制处理。
//
// srcStrideBytes 是源每一行的字节数（DXGI 的 RowPitch 通常大于 width*4）。
// 入参非法（空指针、宽高非正、stride 小于一行）时返回空数组。
QByteArray bgraToNv12(const uchar *src, int srcStrideBytes, int width, int height);

// NV12 -> QImage(Format_ARGB32)，bgraToNv12 的逆变换（同样按 BT.601 有限范围）。
// yStride 是 Y 平面每行的字节数；色度平面按同样跨距处理（紧凑 NV12 时二者相等）。
// 入参非法时返回空 QImage。
QImage nv12ToArgb32(const uchar *nv12, int yStride, int width, int height);

// NV12 -> QImage(Format_ARGB32)，但**直接输出到缩小后的尺寸**。
//
// 动机（压测实测）：4K 单帧 37.5 ms 里色彩转换占 28.9 ms（77%），瓶颈是「往全尺寸
// ARGB32 写 33 MB 内存」，不是 H.264 解码本体（只 8.6 ms）。一控多时窗口格子的物理
// 像素远小于 4K，全尺寸转换纯属白烧带宽 —— 本函数在转换循环里顺手把尺寸降下来。
//
// 缩放规则：
// * 以 boxW/boxH 为**上限框**，取统一比例因子 s = min(boxW/srcW, boxH/srcH) 等比缩放
// —— 保证输出宽高比与源一致（渲染器的归一化坐标映射依赖这个前提）。
// * **永不放大**（s 上限 1.0）：放大交给 QPainter 做，避免多写一遍内存。
// * 缩小用 box 平均（每个目的像素覆盖的源矩形求平均），避免细文字出现锯齿。
// * **收益判定**：输出面积 > 源的 **1/4** 时**不做缩放**、直接返回全尺寸 —— box 平均要给
// 每个目的像素额外交「4 次取样 + 乘倒数」。2026-11-01 重测：面积比 1/4 上缩放仍快
// 1.13x（1440p）~ 1.29x（4K），1/9 上快 2.3x；1/4 以上没测，故不越界。
// 不满足就退回全尺寸，那点缩小交给 QPainter（4K→1920x1080 平滑 3.61 ms/帧，
// 但那 3.61 ms 是 **UI 线程**的活，回退并非免费）。
// ⇒ 本函数**可能返回比 box 更大的图**，调用方别假设尺寸被夹住。
// * boxW/boxH 非法（<=0）时回退到全尺寸 —— 宁可慢，不能让画面缩成 0 像素消失。
// * nv12 为空 / 宽高非正 / yStride 小于一行时返回空 QImage（与 nv12ToArgb32 同口径）。
QImage nv12ToArgb32Scaled(const uchar *nv12,
                          int          yStride,
                          int          srcWidth,
                          int          srcHeight,
                          int          boxWidth,
                          int          boxHeight);

} // namespace lanrdp
