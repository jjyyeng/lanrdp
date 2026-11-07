#include "app/TaskbarBadge.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QWidget>

#include <cstring>

namespace lanrdp {

namespace {

ITaskbarList3 *asTaskbar(void *raw)
{
    return static_cast<ITaskbarList3 *>(raw);
}

// 角标底色。与界面主题里的「危险」色一致（错误红字 / 密码错误用的是同一个色相）。
const QColor kBadgeColor(0xE2, 0x4B, 0x4A);

// 画一个红底 + 数字的小圆。size 是**物理像素**：高 DPI 下任务栏会按更大的
// 尺寸取图标，只画 16×16 会被拉伸发虚。
QImage badgeImage(int count, qreal dpr)
{
    const int px = qMax(16, qRound(16.0 * (dpr > 0.0 ? dpr : 1.0)));

    QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(kBadgeColor);
    p.drawEllipse(QRectF(0.5, 0.5, px - 1.0, px - 1.0));

    QFont f = p.font();
    f.setPixelSize(qMax(8, qRound(px * 0.62)));
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(img.rect(), Qt::AlignCenter, QString::number(count));
    p.end();

    return img;
}

// QImage → HICON。Windows 的 32bpp 位图按**预乘 alpha** 解释，所以进来先转格式；
// 位图自上而下逐行拷（DIB 高度取负值就是这个含义），避免依赖 stride 恰好相等。
HICON imageToHIcon(const QImage &source)
{
    if (source.isNull())
        return nullptr;

    const QImage img = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int      w = img.width();
    const int      h = img.height();
    if (w <= 0 || h <= 0)
        return nullptr;

    BITMAPV5HEADER header = {};
    header.bV5Size        = sizeof(header);
    header.bV5Width       = w;
    header.bV5Height      = -h;
    header.bV5Planes      = 1;
    header.bV5BitCount    = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask     = 0x00FF0000;
    header.bV5GreenMask   = 0x0000FF00;
    header.bV5BlueMask    = 0x000000FF;
    header.bV5AlphaMask   = 0xFF000000;

    void *bits = nullptr;
    HDC   dc   = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&header),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!colour || !bits)
        return nullptr;

    const int rowBytes = w * 4;
    const uchar *src   = img.constBits();
    uchar       *dst   = static_cast<uchar *>(bits);
    for (int y = 0; y < h; ++y)
        std::memcpy(dst + y * rowBytes, src + y * img.bytesPerLine(), size_t(rowBytes));

    // 32bpp 图标仍要求一张掩码位图；有了 alpha 通道它实际不参与合成。
    HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);

    ICONINFO info = {};
    info.fIcon    = TRUE;
    info.hbmColor = colour;
    info.hbmMask  = mask;

    HICON icon = CreateIconIndirect(&info);

    DeleteObject(colour);
    if (mask)
        DeleteObject(mask);

    return icon;
}

} // namespace

TaskbarBadge::TaskbarBadge(QWidget *window)
    : window_(window)
{
    // 界面线程通常已经被 Qt 初始化成 STA；返回 S_FALSE 表示「早就初始化过」，
    // 这一份引用仍然要配一次 CoUninitialize。模型冲突（RPC_E_CHANGED_MODE）
    // 说明别人用的是 MTA，那就照常往下走 —— 拿不到接口也只是没有角标。
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hrInit))
        ownsCom_ = true;

    ITaskbarList3 *bar = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_ITaskbarList3,
                                        reinterpret_cast<void **>(&bar));
    if (SUCCEEDED(hr) && bar) {
        if (SUCCEEDED(bar->HrInit())) {
            taskbar_ = bar;
        } else {
            bar->Release();
        }
    }
}

TaskbarBadge::~TaskbarBadge()
{
    if (taskbar_) {
        // 窗口可能已经销毁，此时句柄无效 —— SetOverlayIcon 会失败，不必理会，
        // 但要保证图标资源回收。
        asTaskbar(taskbar_)->Release();
        taskbar_ = nullptr;
    }

    if (icon_) {
        DestroyIcon(static_cast<HICON>(icon_));
        icon_ = nullptr;
    }

    if (ownsCom_)
        CoUninitialize();
}

void TaskbarBadge::setCount(int count, const QString &description)
{
    const int wanted = count > 0 ? count : 0;
    if (wanted == count_)
        return;

    // 本来就没有角标、这次也还是「没有」：不必为了清空去创建原生窗口。
    if (wanted == 0 && icon_ == nullptr) {
        count_ = 0;
        return;
    }

    if (!taskbar_ || !window_)
        return;

    const HWND hwnd = reinterpret_cast<HWND>(window_->winId());
    if (!hwnd)
        return;

    HICON next = nullptr;
    if (wanted > 0)
        next = imageToHIcon(badgeImage(wanted, window_->devicePixelRatioF()));

    const LPCWSTR tip = description.isEmpty()
                            ? nullptr
                            : reinterpret_cast<LPCWSTR>(description.utf16());
    asTaskbar(taskbar_)->SetOverlayIcon(hwnd, next, tip);

    // SetOverlayIcon 不接管所有权，旧图标要自己销毁。
    if (icon_)
        DestroyIcon(static_cast<HICON>(icon_));
    icon_  = next;
    count_ = wanted;
}

void TaskbarBadge::flash()
{
    if (!window_)
        return;

    const HWND hwnd = reinterpret_cast<HWND>(window_->winId());
    if (!hwnd || GetForegroundWindow() == hwnd)
        return;

    FLASHWINFO info = {};
    info.cbSize     = sizeof(info);
    info.hwnd       = hwnd;
    info.dwFlags    = FLASHW_TRAY;   // 只闪任务栏按钮，不抢焦点、不动窗口本身
    info.uCount     = 6;
    info.dwTimeout  = 0;
    FlashWindowEx(&info);
}

} // namespace lanrdp
