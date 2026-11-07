#include "app/TrayController.h"

#include "app/Glyph.h"
#include "app/Theme.h"
#include "app/WindowStatus.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QSystemTrayIcon>
#include <QTimer>

#include <cstring>

namespace lanrdp {

namespace {

// 状态点的红。与任务栏角标同一个色值 —— 两处标记讲的是同一件事（有人正在
// 看本机），颜色不同会让人以为是两种告警。
const QColor kSharedDot(0xE2, 0x4B, 0x4A);

// 交付给 QIcon 的物理尺寸档。Windows 按 DPI 挑：100% 用 16、125% 用 20、
// 150% 用 24、200% 用 32。只给 16 会被系统拉伸，在 150% 的机器上发虚。
constexpr int kTraySizes[] = {16, 20, 24, 32, 40};

} // namespace

QString trayToolTip(TrayStatus status, int peerCount, const QString &peer)
{
    if (status == TrayStatus::Listening || peerCount <= 0)
        return hostWindowTitle(true, 0, QString());

    // 与窗口标题共用一套措辞：两处说法对不上时，人会以为自己看的是两件事。
    return hostWindowTitle(true, peerCount, peer);
}

QPixmap trayIconPixmap(TrayStatus status, int pixels)
{
    const int px = qMax(8, pixels);

    QPixmap pm(px, px);
    pm.fill(Qt::transparent);

    // 底座用「显示器」轮廓：托盘里 16px 也认得出是个屏幕。有人正在看时提亮，
    // 与「等着」形成可辨的差别 —— 光靠一个小红点在 16px 下不够稳。
    const QColor base = status == TrayStatus::Shared ? theme::colors().textPrimary
                                                     : theme::colors().textSecondary;
    const QPixmap screen = glyph::pixmap(glyph::Shape::Monitor, px, 1.0, base);

    QPainter p(&pm);
    p.drawPixmap(0, 0, screen);

    if (status == TrayStatus::Shared) {
        // 右上角一个实心点。半径按边长比例走，16px 下约 3.5px —— 再小就糊成一团。
        const qreal r = qMax(2.0, px * 0.22);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(kSharedDot);
        p.drawEllipse(QPointF(px - r, r), r, r);
    }
    p.end();

    return pm;
}

TrayController::TrayController(QObject *parent)
    : QObject(parent)
{
}

TrayController::~TrayController()
{
    // 菜单没有父对象（QMenu 的父对象只能是 QWidget，这一层拿不到合适的），
    // 得自己收；图标挂在 this 下，随 QObject 析构一并销毁。
    delete menu_;
    menu_ = nullptr;
}

bool TrayController::available()
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

bool TrayController::isVisible() const
{
    return icon_ && icon_->isVisible() && shellHasIcon_;
}

void TrayController::setVisible(bool on)
{
    if (!on) {
        if (icon_)
            icon_->hide();
        shellHasIcon_ = false;
        probeRetries_ = 0;
        return;
    }

    // ⚠️ 不做「请求过就跳过」的短路：上一次可能压根没建出图标（通知区域还没就绪、
    // 进程完整性不足……），一短路就再没有补上的机会。下面这些平台调用都是幂等的。
    ensureIcon();
    if (!icon_)
        return;

    refreshIcon();
    if (!icon_->isVisible())
        icon_->setVisible(true);

    probeRetries_ = 0;
    probeShell();
}

// 向 shell 求证：Qt 的托盘消息窗口（标题固定；类名带 Qt 版本号，按标题找更稳）
// 名下到底有没有我们的图标。
//
// `Shell_NotifyIconGetRect` 的返回值是唯一可信的判据：成功就说明图标真在通知
// 区域里（在溢出区时给的是溢出按钮那个矩形），失败说明压根没登记进去。
bool TrayController::shellKnowsIcon()
{
    const HWND window = ::FindWindowW(nullptr, L"QTrayIconMessageWindow");
    if (!window)
        return false;

    // 多开时窗口不止一个，按标题找到的可能属于另一个进程 —— 那不是我们的图标。
    DWORD owner = 0;
    ::GetWindowThreadProcessId(window, &owner);
    if (owner != ::GetCurrentProcessId())
        return false;

    using GetRectFn = HRESULT(WINAPI *)(const void *, RECT *);
    static const GetRectFn fn = [] {
        const HMODULE shell = ::GetModuleHandleW(L"shell32.dll");
        if (!shell)
            return GetRectFn(nullptr);
        const FARPROC raw = ::GetProcAddress(shell, "Shell_NotifyIconGetRect");
        // 用 memcpy 搬运而不是强转函数指针 —— 后者会触发 -Wcast-function-type，
        // 而这里本来就是在做「按名字取一个签名已知的函数」这种有意的转换。
        GetRectFn resolved = nullptr;
        static_assert(sizeof(resolved) == sizeof(raw), "函数指针宽度对不上");
        std::memcpy(&resolved, &raw, sizeof(resolved));
        return resolved;
    }();
    if (!fn)
        return false;

    // 与 shell 约定的 NOTIFYICONIDENTIFIER 同布局；自己写一份，免得依赖 SDK 版本
    //（这个类型在部分 MinGW 头里没有）。
    struct Identifier {
        DWORD cbSize;
        HWND  hWnd;
        UINT  uID;
        GUID  guidItem;
    } id {};
    id.cbSize = sizeof(id);
    id.hWnd   = window;
    id.uID    = 0;   // Qt 的所有图标共用一个消息窗口，靠 uID 区分，它用的是 0

    RECT rect {};
    return SUCCEEDED(fn(&id, &rect));
}

void TrayController::probeShell()
{
    shellHasIcon_ = shellKnowsIcon();

    // 登记是异步的：刚 setVisible(true) 时 shell 往往还没处理完，隔一会儿再问。
    // 但只问有限几次 —— 真挂不上时（权限不够）问多少次都是白问。
    if (!shellHasIcon_ && probeRetries_ < 4 && icon_ && icon_->isVisible()) {
        ++probeRetries_;
        QTimer::singleShot(250, this, [this]() { probeShell(); });
    }
}

void TrayController::ensureIcon()
{
    if (icon_)
        return;

    // 没有通知区域就到此为止：留空 icon_，后面所有平台调用自然变成空转。
    if (!available())
        return;

    icon_ = new QSystemTrayIcon(this);

    menu_ = new QMenu();
    menu_->setObjectName(QStringLiteral("trayMenu"));

    QAction *restore = menu_->addAction(QStringLiteral("显示主窗口"));
    restore->setObjectName(QStringLiteral("trayRestore"));
    connect(restore, &QAction::triggered, this, &TrayController::restoreRequested);

    QAction *stop = menu_->addAction(QStringLiteral("停止共享"));
    stop->setObjectName(QStringLiteral("trayStopSharing"));
    connect(stop, &QAction::triggered, this, &TrayController::stopSharingRequested);

    menu_->addSeparator();

    QAction *quit = menu_->addAction(QStringLiteral("退出"));
    quit->setObjectName(QStringLiteral("trayQuit"));
    connect(quit, &QAction::triggered, this, &TrayController::quitRequested);

    icon_->setContextMenu(menu_);

    connect(icon_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                // 双击是惯例；单击也认，因为 Windows 通知区域的单击在部分
                // 外壳实现里会报成 Trigger 而不是 DoubleClick。
                if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
                    emit restoreRequested();
            });
}

void TrayController::refreshIcon()
{
    if (!icon_)
        return;

    QIcon icon;
    for (int px : kTraySizes)
        icon.addPixmap(trayIconPixmap(status_, px));

    icon_->setIcon(icon);
    icon_->setToolTip(trayToolTip(status_, peerCount_, peer_));
}

void TrayController::setStatus(TrayStatus status, int peerCount, const QString &peer)
{
    status_    = status;
    peerCount_ = qMax(0, peerCount);
    peer_      = peer;

    refreshIcon();
}

TrayStatus TrayController::status() const
{
    return status_;
}

int TrayController::peerCount() const
{
    return peerCount_;
}

void TrayController::notify(const QString &title, const QString &body)
{
    // 图标根本没挂上时不必弹：气泡说的是「去托盘里找我」，而托盘里什么都没有。
    if (!isVisible())
        return;

    icon_->showMessage(title, body, QSystemTrayIcon::Information, 6000);
}

} // namespace lanrdp
