#include "host/ColorConvert.h"

#include <QVector>

#include <cmath>

namespace lanrdp {

namespace {

// BT.601 整数定点系数（等价于 0.257/0.504/0.098 与 0.439/0.291/0.368 那套浮点式）
inline int clampToRange(int value, int lo, int hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

inline int rgbToY(int r, int g, int b)
{
    return clampToRange(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 16, 235);
}

inline int rgbToCb(int r, int g, int b)
{
    return clampToRange(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 16, 240);
}

inline int rgbToCr(int r, int g, int b)
{
    return clampToRange(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 16, 240);
}

} // namespace

QByteArray bgraToNv12(const uchar *src, int srcStrideBytes, int width, int height)
{
    if (src == nullptr || width <= 0 || height <= 0)
        return {};
    if (srcStrideBytes < width * 4)
        return {};

    const int chromaWidth  = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;

    QByteArray out;
    out.resize(width * height + chromaWidth * chromaHeight * 2);

    uchar *yPlane  = reinterpret_cast<uchar *>(out.data());
    uchar *uvPlane = yPlane + static_cast<qsizetype>(width) * height;

    // ---- Y 平面：逐像素，无分支，便于自动向量化 ----
    for (int y = 0; y < height; ++y) {
        const uchar *row = src + static_cast<qsizetype>(y) * srcStrideBytes;
        uchar       *dst = yPlane + static_cast<qsizetype>(y) * width;
        for (int x = 0; x < width; ++x) {
            const int b = row[x * 4 + 0];
            const int g = row[x * 4 + 1];
            const int r = row[x * 4 + 2];
            dst[x] = static_cast<uchar>(rgbToY(r, g, b));
        }
    }

    // ---- UV 平面：2x2 求平均后再转换（先在 RGB 域平均比在 YUV 域平均更准）----
    for (int cy = 0; cy < chromaHeight; ++cy) {
        for (int cx = 0; cx < chromaWidth; ++cx) {
            int sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (int dy = 0; dy < 2; ++dy) {
                int sy = cy * 2 + dy;
                if (sy >= height)
                    sy = height - 1;   // 奇数高度：边缘复制
                const uchar *row = src + static_cast<qsizetype>(sy) * srcStrideBytes;
                for (int dx = 0; dx < 2; ++dx) {
                    int sx = cx * 2 + dx;
                    if (sx >= width)
                        sx = width - 1; // 奇数宽度：边缘复制
                    sumB += row[sx * 4 + 0];
                    sumG += row[sx * 4 + 1];
                    sumR += row[sx * 4 + 2];
                    ++count;
                }
            }
            const int r = sumR / count;
            const int g = sumG / count;
            const int b = sumB / count;

            const qsizetype base = (static_cast<qsizetype>(cy) * chromaWidth + cx) * 2;
            uvPlane[base + 0] = static_cast<uchar>(rgbToCb(r, g, b));
            uvPlane[base + 1] = static_cast<uchar>(rgbToCr(r, g, b));
        }
    }

    return out;
}

namespace {

// BT.601 有限范围的反变换整数系数
inline int clamp8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

} // namespace

QImage nv12ToArgb32(const uchar *nv12, int yStride, int width, int height)
{
    if (nv12 == nullptr || width <= 0 || height <= 0)
        return {};
    if (yStride < width)
        return {};

    const uchar *yPlane  = nv12;
    const uchar *uvPlane = nv12 + static_cast<qsizetype>(yStride) * height;

    QImage image(width, height, QImage::Format_ARGB32);
    // 构造失败（内存不足 / 尺寸被 Qt 拒绝）时 image 为 null，scanLine() 返回空指针 ⇒ memcpy 崩。
    if (image.isNull())
        return {};

    for (int y = 0; y < height; ++y) {
        auto        *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
        const uchar *yRow = yPlane + static_cast<qsizetype>(y) * yStride;
        const uchar *uvRow = uvPlane + static_cast<qsizetype>(y / 2) * yStride;

        for (int x = 0; x < width; ++x) {
            const int c = x - (x % 2);   // 色度按 2 像素共享
            const int Y = yRow[x] - 16;
            const int U = uvRow[c] - 128;
            const int V = uvRow[c + 1] - 128;

            const int r = clamp8((298 * Y + 409 * V + 128) >> 8);
            const int g = clamp8((298 * Y - 100 * U - 208 * V + 128) >> 8);
            const int b = clamp8((298 * Y + 516 * U + 128) >> 8);
            dst[x]      = qRgba(r, g, b, 255);
        }
    }
    return image;
}

// ---------------------------------------------------------------------------
// 转换 + 缩放（一次遍历里同时降尺寸，省掉「写全尺寸 ARGB32」）
// ---------------------------------------------------------------------------

QImage nv12ToArgb32Scaled(const uchar *nv12,
                          int          yStride,
                          int          srcWidth,
                          int          srcHeight,
                          int          boxWidth,
                          int          boxHeight)
{
    if (nv12 == nullptr || srcWidth <= 0 || srcHeight <= 0)
        return {};
    if (yStride < srcWidth)
        return {};

    // 目标框被压到 0/负：回退全尺寸。**绝不能**在这儿算出 0 像素的图，
    // 那会让画面整块消失（渲染器收到空帧只会黑屏）。
    if (boxWidth <= 0 || boxHeight <= 0)
        return nv12ToArgb32(nv12, yStride, srcWidth, srcHeight);

    // 统一比例因子：等比缩放进框，且**永不放大**（上限 1.0）
    const double sx = static_cast<double>(boxWidth) / static_cast<double>(srcWidth);
    const double sy = static_cast<double>(boxHeight) / static_cast<double>(srcHeight);
    double       s  = sx < sy ? sx : sy;
    if (s > 1.0)
        s = 1.0;

    int dstWidth  = static_cast<int>(std::lround(static_cast<double>(srcWidth) * s));
    int dstHeight = static_cast<int>(std::lround(static_cast<double>(srcHeight) * s));
    if (dstWidth < 1)
        dstWidth = 1;
    if (dstHeight < 1)
        dstHeight = 1;

    // 因子被夹到 1 后仍是原尺寸：走原路径（省掉 box 循环里那些除法）
    if (dstWidth >= srcWidth && dstHeight >= srcHeight)
        return nv12ToArgb32(nv12, yStride, srcWidth, srcHeight);

    // ⚠️ 收益判定（2026-11-01 重测，阈值由 1/9 放宽到 1/4）：
    // box 平均要给每个目的像素额外交「4 次取样 + 乘倒数」的开销。映射表**预计算之前**
    // 它连很小的面积比都跑不过全尺寸（见下面那段），预计算之后重新扫：
    //
    // 面积比         1/4      ≈0.21     1/9
    // 4K            1.29x     1.52x    2.36x
    // 1440p         1.13x     1.45x    2.29x      x = 全尺寸耗时 ÷ 缩放耗时
    //
    // ⇒ 到面积比 1/4 为止，缩放都比全尺寸转换**更快**，所以阈值取 1/4（含等号）。
    // 以前这里写「盈亏平衡在 1/9、1/4 慢一倍」，依据是 1/4 → 0.53x 那张表 —— 那是
    // 映射表预计算之前的产物，方向都反了，**别再引用**。
    // 1/4 以上没测，所以不越界：面积比 > 1/4 一律退回全尺寸，交给 QPainter 去缩。
    //
    // 这条线现在**只决定性能**，不影响画质：超过阈值退回全尺寸后，绘制期一定开平滑
    // （双线性），不存在「解码没缩 + 绘制也不平滑」的最近邻档。所以两处阈值再出现
    // 不一致，最多是多花一点绘制时间，不会把画面变糊。
    if (static_cast<qint64>(dstWidth) * dstHeight * 4
        > static_cast<qint64>(srcWidth) * srcHeight)
        return nv12ToArgb32(nv12, yStride, srcWidth, srcHeight);

    const uchar *yPlane  = nv12;
    const uchar *uvPlane = nv12 + static_cast<qsizetype>(yStride) * srcHeight;

    const int chromaHeight = (srcHeight + 1) / 2;
    const int chromaMaxX   = ((srcWidth + 1) / 2) * 2 - 2;  // 最后一个 UV 对的 U 下标

    // ⚠️ 映射表必须**预计算**。压测实测（2026-09-24）：把 x0/x1 的整除和 sumY/count
    // 的平均除法留在内层循环里，4K→3420x1924 要跑 658 万次目的像素 × 3 次整除，
    // 结果**比全尺寸转换还慢 2.3 倍**（54.6 ms vs 24.0 ms）—— 收益被除法全部吃掉。
    // 挪出来之后只剩乘加，缩放才真的变快。
    QVector<int>    colStart(dstWidth), colCount(dstWidth);
    QVector<double> invCol(dstWidth);
    for (int dx = 0; dx < dstWidth; ++dx) {
        const int x0 = dx * srcWidth / dstWidth;
        int       x1 = (dx + 1) * srcWidth / dstWidth;
        if (x1 <= x0)
            x1 = x0 + 1;
        colStart[dx] = x0;
        colCount[dx] = x1 - x0;
        invCol[dx]   = 1.0 / static_cast<double>(x1 - x0);
    }

    QVector<int>    rowStart(dstHeight), rowCount(dstHeight), chromaRow(dstHeight);
    QVector<double> invRow(dstHeight);
    for (int dy = 0; dy < dstHeight; ++dy) {
        const int y0 = dy * srcHeight / dstHeight;
        int       y1 = (dy + 1) * srcHeight / dstHeight;
        if (y1 <= y0)
            y1 = y0 + 1;
        rowStart[dy] = y0;
        rowCount[dy] = y1 - y0;
        invRow[dy]   = 1.0 / static_cast<double>(y1 - y0);

        // 色度是 2x2 共享的低频信息：取覆盖带中间那一行就够了
        int cy = ((y0 + y1) >> 1) / 2;
        if (cy < 0)
            cy = 0;
        if (cy >= chromaHeight)
            cy = chromaHeight - 1;
        chromaRow[dy] = cy;
    }

    QImage image(dstWidth, dstHeight, QImage::Format_ARGB32);
    // 同上：构造失败会留下 null 图，后面的 scanLine() 就是空指针解引用。
    if (image.isNull())
        return {};

    for (int dy = 0; dy < dstHeight; ++dy) {
        const int    nRows    = rowCount[dy];
        const int    y0       = rowStart[dy];
        const int    y1       = y0 + nRows;
        const uchar *uvRow    = uvPlane + static_cast<qsizetype>(chromaRow[dy]) * yStride;
        const double invRowDy = invRow[dy];

        auto *dst = reinterpret_cast<QRgb *>(image.scanLine(dy));
        for (int dx = 0; dx < dstWidth; ++dx) {
            const int x0    = colStart[dx];
            const int x1    = x0 + colCount[dx];
            const int nCols = colCount[dx];

            int Y;
            if (nCols == 1 && nRows == 1) {
                // 缩得极轻（或 1:1）时覆盖区就一个像素 —— 直接取样，
                // 省掉求和与平均（这条捷径覆盖了近 1:1 缩放的绝大多数像素）
                Y = yPlane[static_cast<qsizetype>(y0) * yStride + x0] - 16;
            } else {
                // Y：覆盖矩形内求平均 ⇒ box 滤波（缩小时的抗锯齿）。
                // 除法用「乘倒数」替掉：sumY/n 是整数除，比浮点乘慢一个数量级。
                int sumY = 0;
                for (int ry = y0; ry < y1; ++ry) {
                    const uchar *row = yPlane + static_cast<qsizetype>(ry) * yStride;
                    for (int rx = x0; rx < x1; ++rx)
                        sumY += row[rx];
                }
                Y = static_cast<int>(static_cast<double>(sumY) * invRowDy * invCol[dx] + 0.5) - 16;
            }

            // UV：取覆盖带中心并对齐到偶数（NV12 每 2x2 共享一对）
            int cx = ((x0 + x1) >> 1) & ~1;
            if (cx > chromaMaxX)
                cx = chromaMaxX;
            if (cx < 0)
                cx = 0;
            const int U = uvRow[cx] - 128;
            const int V = uvRow[cx + 1] - 128;

            const int r = clamp8((298 * Y + 409 * V + 128) >> 8);
            const int g = clamp8((298 * Y - 100 * U - 208 * V + 128) >> 8);
            const int b = clamp8((298 * Y + 516 * U + 128) >> 8);
            dst[dx]     = qRgba(r, g, b, 255);
        }
    }
    return image;
}

} // namespace lanrdp
