#pragma once

#include <QIcon>
#include <QObject>
#include <QPixmap>
#include <QString>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace lanrdp {

// 托盘图标要表达的状态。
//
// 只有「正在被控」和「在等客」两档 —— 空闲时托盘根本不存在（开关关着就
// 不创建），所以没有第三档。
enum class TrayStatus {
    Listening,  // 在监听，但还没人连
    Shared,     // 有人正在看
};

// 通知区域的常驻图标。
//
// 只有在「关闭窗口后继续共享」打开时才会被创建；关着的时候这个类只是个
// 状态记录器，不碰任何平台接口 —— 默认关的开关不该留下任何痕迹。
//
// 无托盘的环境（精简 shell、部分远程会话）里 available() 为 false，此时
// 所有平台调用空转，但 setStatus 照常记录：上层要靠这个记录判断「关掉窗口
// 会不会变成看不见的进程」。
class TrayController : public QObject
{
    Q_OBJECT

public:
    explicit TrayController(QObject *parent = nullptr);
    ~TrayController() override;

    // 这台机器有没有通知区域。qoffscreen 平台下恒为 false。
    static bool available();

    // 图标是否**真的**在通知区域里。
    //
    // ⚠️ 不是 `QSystemTrayIcon::isVisible()` —— 那个只表示「我希望它可见」。
    // 图标被外壳拒收时（例如进程完整性过低，`Shell_NotifyIcon` 被 UIPI 拦下）
    // 它照样返回 true，上层据此把窗口藏掉，就落得「图标没有、窗口也没了」。
    // 这里额外向 shell 求证一次，问不到就当没挂上。
    bool isVisible() const;

    void setVisible(bool on);

    // 记录状态并刷新图标与悬停提示。不可见时只记录。
    void setStatus(TrayStatus status, int peerCount = 0, const QString &peer = QString());

    TrayStatus status() const;
    int        peerCount() const;

    // 短暂气泡提示。不可见时什么也不做。
    void notify(const QString &title, const QString &body);

    // 按当前界面语言重设菜单项文字与悬停提示。语言切换时由 MainWindow 调用 ——
    // 托盘菜单是 QMenu + QAction，收不到 QEvent::LanguageChange（它不是控件）。
    void refreshTexts();

    // 换了主题：按当前色板重画图标。
    //
    // 托盘图标是位图，颜色在生成时就定了。不重画的话，浅色主题下通知区域里
    // 还挂着一枚深色主题的浅色显示器 —— 在浅色任务栏上基本看不见。
    void applyTheme();

signals:
    void restoreRequested();      // 双击图标，或菜单【显示主窗口】
    void stopSharingRequested();  // 菜单【停止共享】
    void quitRequested();         // 菜单【退出】

private:
    void ensureIcon();
    void refreshIcon();

    // 向 shell 求证图标在不在，并把结果记进 shellHasIcon_。
    // 登记是异步的，所以第一次问不到就隔一会儿再问几次。
    void probeShell();
    static bool shellKnowsIcon();

    QSystemTrayIcon *icon_ = nullptr;
    QMenu           *menu_ = nullptr;
    // 菜单项留指针：菜单是建一次用到底的，语言切换要把文字重设一遍。
    QAction         *restoreAction_   = nullptr;
    QAction         *stopAction_      = nullptr;
    QAction         *quitAction_      = nullptr;
    TrayStatus       status_    = TrayStatus::Listening;
    int              peerCount_ = 0;
    QString          peer_;

    // shell 是否认得这个图标。QSystemTrayIcon 自己说不准，只能另外问。
    bool shellHasIcon_ = false;
    int  probeRetries_ = 0;
};

// 悬停提示的文案。与窗口标题同口径 —— 只有一个连接时点名道姓，多个时报数，
// 两处说法不一致会让人以为两处在讲不同的事。
QString trayToolTip(TrayStatus status, int peerCount, const QString &peer);

// 托盘图标的位图。pixels 是**物理像素**边长：QIcon 按 DPI 挑档，所以这里
// 不收 DPR，免得同一张图被乘两次（乘两次会让位图超出目标尺寸，画出来是空的）。
QPixmap trayIconPixmap(TrayStatus status, int pixels);

} // namespace lanrdp
