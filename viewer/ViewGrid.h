#pragma once

#include <QVector>
#include <QWidget>

class QEvent;
class QGridLayout;

namespace lanrdp {

// 一格在网格里的落位：行列索引 + 跨度。行列都从 0 起。
struct GridCell
{
    int row     = 0;
    int col     = 0;
    int rowSpan = 1;
    int colSpan = 1;
};

// 一套网格布局的完整描述：列数 + 每格的落位。
// `cells` 的**顺序即视窗顺序** —— 第 i 个元素对应第 i 个视窗。
struct GridSpec
{
    int               columns = 1;
    QVector<GridCell> cells;
};

// 多视窗承载：一个 QGridLayout 宿主，按视窗数自动选布局，并支持「任一格占满」。
//
// 为什么单独成类，而不是把 QGridLayout 直接塞进 MainWindow：
// · 布局规则是**纯函数** `layoutForCount()` —— 能脱离 UI 单测（见 tests/test_viewgrid.cpp）；
// · 「占满 / 还原」「选中哪一格」是有状态的视图逻辑，收在这里之后，
// MainWindow 只剩「往里头放哪些视窗」一件事。
//
// 与 ScreenRenderer 的关系：本类**不认识**它，只当视窗是普通 QWidget ——
// 画面怎么画是 renderer 的事。这样它也能装测试用的占位控件。
//
// ⚠️ 它**只管显隐与位置，从不销毁视窗**：占满时其余格子只是 `hide()`，
// 对象与连接都还在（帧照收），切回来立刻有画面。什么时候销毁由调用方决定。
class ViewGrid : public QWidget
{
    Q_OBJECT

public:
    explicit ViewGrid(QWidget *parent = nullptr);

    // ---- 纯函数：布局规则（用户已定档）----
    // 1 → 1×1 ｜ 2 → 1×2 ｜ 3 → 左大右两小 ｜ 4 → 2×2
    // 本项目上限 4 台；越界时退化为「2 列瀑布」（继续往下排行号递增，仍不重叠）。
    static GridSpec layoutForCount(int count);

    // ---- 视窗集合 ----
    // 重排：先把上一批全部 `removeWidget`，再按新布局放回去。
    // ⚠️ 必须「先摘后加」—— 同一个 widget 在 QGridLayout 里重复 addWidget
    // 属于未定义行为（旧位置仍在，会出现错位 / 幽灵格子）。
    // 「占满」与「选中」按**视窗身份**迁移到新下标（名单中间少一格会让后面的全部前移，
    // 只看下标就会漂到别的格上）；那一格已经不在了才退回「无占满 / 原位钳进范围」。
    void setViews(const QVector<QWidget *> &views);

    const QVector<QWidget *> &views() const { return views_; }
    int       viewCount() const { return views_.size(); }
    QWidget  *viewAt(int index) const;
    // 视窗在名单里的下标；不在名单里（或传 nullptr）返回 -1。
    int       indexOfView(QWidget *view) const;

    // ---- 「占满 / 还原」----
    // index 越界（或给 -1）即还原。占满 = 该格跨满整个网格 + 其余 hide()。
    void setMaximizedIndex(int index);
    int  maximizedIndex() const { return maximized_; }
    bool isMaximized() const { return maximized_ >= 0 && maximized_ < views_.size(); }

    // ---- 选中（「点哪一格就操作哪一格」）----
    // 默认第 0 格；越界自动钳到 [0, count-1]。选中只是视图状态，不改变任何连接。
    int  selectedIndex() const { return selected_; }
    void setSelectedIndex(int index);

signals:
    void selectedChanged(int index);

protected:
    // 旁观点击：记下「用户在操作哪一格」，**不消费**事件。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void relayout();

    QGridLayout        *grid_      = nullptr;
    QVector<QWidget *>  views_;
    int                 maximized_ = -1;
    int                 selected_  = 0;
};

} // namespace lanrdp
