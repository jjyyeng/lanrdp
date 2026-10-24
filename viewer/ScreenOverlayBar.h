#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

class QContextMenuEvent;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QToolButton;
class QWheelEvent;

namespace lanrdp {

// 视窗顶部悬浮操作条。
//
// 它是**独立子控件**（挂在画面控件之下、覆盖其顶部）而不是画面控件里的一段事件
// 处理 —— 这不是随手选的实现，而是被三条实测约束逼出来的：
// · 画面区的右键**已经属于远端**（远端右键菜单是最高频动作之一），本地不能抢；
// · `QWidget::mouseDoubleClickEvent()` 的默认实现**会转手再调一次**
// `mousePressEvent()` ⇒ 拿双击做本地动作时，第二下照样被转发给远端；
// · `Escape` 在 `viewer/KeyMap.cpp` 里被映射成 0x1B 且 `keyChanged` 无条件转发，
// 连"未持令牌"这条路径都会顺手发一帧令牌申请。
// ⇒ 唯一的干净办法：让本地手势落在**事件根本到不了画面控件**的地方。
//
// 本类刻意很薄：条高固定、贴顶满宽、跟随父控件尺寸；内容（标题 / 按钮 / 菜单）
// 全部由调用方通过 title() / actionLayout() / setMenu() 填入 —— 因为「被控端」与
// 「控制端」的条内容不同，还会**每条连接一份**。
class ScreenOverlayBar : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenOverlayBar(QWidget *parent = nullptr);

    // 条高（**逻辑**像素）。物理像素由 Qt 按 devicePixelRatio 放大 ——
    // 本机 4K@150% 实测：物理 = 逻辑 × 1.5 + 17（dock 边框），条同理。
    static int barHeight();

    // 纯函数：给定父控件尺寸，算出本条应当占据的矩形（贴顶、满宽）。
    // 抽成静态纯函数是为了把「贴顶满宽」这条约束钉在单测里，不必起真窗口。
    static QRect geometryFor(const QSize &parentSize);

    void    setTitle(const QString &text);
    QString title() const;

    // 这一格连接的状态，只画一个 7px 圆点。
    //
    // 一控多时几格标题并排，扫颜色比逐条读「（已断开）」快。
    // ⚠️ 颜色只是**辅助**：标题文字里的状态说明仍然保留 —— 只靠颜色区分状态，
    // 对色弱用户等于没有信息。
    enum class Status {
        Idle,           // 未连接 / 不适用（未选模式、被控端本机）
        Connected,      // 已连上
        Reconnecting,   // 断线后正在自动重连
        Disconnected,   // 已断开
    };
    void   setStatus(Status status);
    Status status() const { return status_; }

    // 这一格是否「被选中」（一控多的焦点指示）。
    // 选中只画**左侧一小段**强调色竖条 —— **不改尺寸**：加边框会挤占画面高度，
    // 让显示预算跟着抖（解码器可能因此重建）。
    // 不用「整条底边横线」：条宽能到 1600+ 像素，一条贯穿全宽的饱和色在深色
    // 画面上比画面本身还抢眼，看起来像装饰条而不是选中标记。
    void setSelected(bool on);
    bool isSelected() const { return selected_; }

    // 右侧动作区。调用方自行 addWidget —— 条不预设任何按钮。
    QHBoxLayout *actionLayout() const { return actions_; }

signals:
    // 双击条（标题区）= 占满 / 还原这一格。
    // 只发信号、不认识「占满」这件事 —— 布局归 ViewGrid / 窗口，条不该知道它。
    // 画面上的双击**收不到这里**：条的按钮是子控件，事件先给它们；而画面的双击属于远端。
    void doubleClicked();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;  // 跟随父控件尺寸
    // 只为把右键盘事件吃掉（否则会冒泡给画面控件 = 在远端弹菜单），不弹任何菜单。
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

    // ⚠️ 下面这一组**必须显式吞掉**（`event->accept()`），不是可有可无的样板：
    // Qt 里**未处理（ignore）的鼠标事件会向上冒泡给父控件**，而条的父控件正是
    // 画面控件 —— 不吞的话「点一下条」等于「点一下远端」，远端会莫名其妙地
    // 被执行操作。这条由 tests/test_mainwindow_ui.cpp 的
    // `overlayBarDoesNotForwardInputToRemote` 钉住（第一版就是漏了它，
    // 实测 5 次点击全部冒泡到了画面控件）。
    // 键盘同理（未处理的 keyPress 也会冒泡，而 Esc 会被无条件转发给远端）。
    // 另加一条 `Qt::WA_NoMousePropagation` 属性做双保险。
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    void relayout();

    QLabel      *title_      = nullptr;
    QHBoxLayout *actions_    = nullptr;
    bool         selected_   = false;
    Status       status_     = Status::Idle;
};

} // namespace lanrdp
