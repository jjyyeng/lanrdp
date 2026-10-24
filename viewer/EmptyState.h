#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

namespace lanrdp {

// 空态层：**未选模式**时盖在画面上的一屏，给两个真入口。
//
// 存在的理由不是好看。从前空态只有一行「未选择模式：从【模式】菜单选…」，
// 用户打开软件的第一屏就是在被指路 —— 现在直接给两个按钮：想操作对方 /
// 想让对方连进来，各按一下即可，菜单只是备份入口。
//
// ⚠️ 它是 `ScreenRenderer` 的子控件，而画面控件会把鼠标 / 键盘事件转发给远端。
// 未连接时 `inputEnabled` 本来就关着（事件到不了远端），但这层仍显式吃掉全部输入 ——
// 与 `ScreenOverlayBar` 同一个理由：不依赖"上游此刻一定是关的"这种假设。
class EmptyState : public QWidget
{
    Q_OBJECT

public:
    explicit EmptyState(QWidget *parent);

    // 「允许对方接入」按钮的双态：未监听 = 允许（弹监听对话框）；
    // 已监听 = 禁止（点击发 hostRequested，由 MainWindow 走停止监听）。
    // 图形 / 文字 / 提示随状态换，按钮仍发同一个信号。
    void setHostListening(bool listening);
    bool isHostListening() const { return hostListening_; }

signals:
    // 用户想操作对方 / 想让对方连进来。由 `MainWindow` 接到对应的对话框。
    void connectRequested();
    void hostRequested();

protected:
    // 每一个都只有一句 `event->accept()`，看着像废话，其实是必需的：
    // QWidget 的默认实现一律 `ignore()`，而 Qt 对**未处理**的鼠标 / 键盘事件
    // 会向上传播给父控件 —— 父控件正是画面控件。
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

    // 跟随父控件（画面）铺满。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLabel      *icon_           = nullptr;
    QLabel      *title_          = nullptr;
    QLabel      *subtitle_       = nullptr;
    QPushButton *connectButton_  = nullptr;
    QPushButton *hostButton_     = nullptr;
    bool         hostListening_  = false;
};

} // namespace lanrdp
