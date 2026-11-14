#pragma once

#include <QObject>
#include <QPointer>

class QKeyEvent;

namespace lanrdp {

class ScreenRenderer;

// 会话期间把 Alt / AltGr 从本机菜单栏手里截下来，直接交给画面控件。
//
// 为什么需要它：Qt 在 Alt **松开**时激活菜单栏，画面控件随即收到
// FocusOut(MenuBarFocusReason)。这一走有两处伤：
//   1. 那一下 KeyRelease 送不到画面控件 ⇒ 远端 Alt 一直停在按下态；
//   2. 之后的按键全部进了菜单栏，一个都不再发往远端。
// 用户看到的就是「按一次 Alt 出按下、要再按一次才出抬起」，中间键盘还断流。
//
// 做法：画面控件持有焦点时，Alt 的按下/抬起由这里转发给它并吞掉（菜单栏看不见）；
// 万一仍被抢走焦点，立刻抢回来。会话之外（对话框、设置）完全不干预，菜单栏照常。
class ModifierKeyGuard : public QObject
{
    Q_OBJECT

public:
    explicit ModifierKeyGuard(QObject *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    static bool isAltKey(const QKeyEvent *event);
    void        relay(ScreenRenderer *renderer, QKeyEvent *event);

    // 按下 Alt 时拥有焦点的画面控件。抬起要回到**同一个**控件上：
    // 期间焦点可能已经换了（点了别的格子），但这一针抬起不能丢。
    QPointer<ScreenRenderer> target_;
    bool                     relaying_ = false;   // 转发进 sendEvent 时别再被自己拦一次
};

} // namespace lanrdp
