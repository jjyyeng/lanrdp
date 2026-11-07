#pragma once

#include <QString>

class QWidget;

namespace lanrdp {

// 任务栏按钮右下角的数字角标 + 任务栏闪烁。
//
// 走 `ITaskbarList3::SetOverlayIcon`，所以**不改程序图标、不新增窗口、不改变
// 「关窗口 = 退出」的语义** —— 这三条正是它相对托盘图标的区别。窗口最小化后
// 任务栏按钮仍然存在，因此角标一直可见。
//
// 拿不到 `ITaskbarList3`（老系统 / COM 起不来）时全部方法静默无操作：
// 角标是锦上添花，绝不能因为它失败而影响监听与画面。
class TaskbarBadge
{
public:
    explicit TaskbarBadge(QWidget *window);
    ~TaskbarBadge();

    TaskbarBadge(const TaskbarBadge &)            = delete;
    TaskbarBadge &operator=(const TaskbarBadge &) = delete;

    // count <= 0 清除角标。description 给读屏器等辅助技术读，不显示在界面上。
    // 幂等：同一数值重复调用不会反复重建图标（状态快照每秒都会推一帧）。
    void setCount(int count, const QString &description);

    // 让任务栏按钮闪几下。窗口已经在前台时不动手 —— 用户正看着它，闪是噪声。
    void flash();

    // 当前角标数值；-1 表示还没设置过。
    int count() const { return count_; }

    // 底层是否真的拿到了任务栏接口（拿不到时角标不生效，测试与排查要看这个）。
    bool available() const { return taskbar_ != nullptr; }

private:
    QWidget *window_  = nullptr;
    void    *taskbar_ = nullptr;   // ITaskbarList3*
    void    *icon_    = nullptr;   // 当前 HICON（SetOverlayIcon 之后归我们所有）
    int      count_   = -1;
    bool     ownsCom_ = false;
};

} // namespace lanrdp
