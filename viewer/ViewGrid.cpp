#include "viewer/ViewGrid.h"

#include <QEvent>
#include <QGridLayout>
#include <QMouseEvent>
#include <QtGlobal>

#include <utility>

namespace lanrdp {

namespace {

// 网格实际占几行 —— 由落位推出来，不额外存一份（少一个能写错的地方）。
int rowsOf(const GridSpec &spec)
{
    int rows = 1;
    for (const GridCell &c : spec.cells)
        rows = qMax(rows, c.row + c.rowSpan);
    return rows;
}

} // namespace

ViewGrid::ViewGrid(QWidget *parent)
    : QWidget(parent)
{
    grid_ = new QGridLayout(this);
    // 格与格之间不留缝：多画面拼接时缝隙是白吃的像素，也让「占满」的边界不好看。
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(0);
}

GridSpec ViewGrid::layoutForCount(int count)
{
    GridSpec spec;

    if (count <= 0)
        return spec;   // 空：columns 留默认 1、cells 为空

    if (count == 1) {  // 1×1
        spec.columns = 1;
        spec.cells.append(GridCell{0, 0, 1, 1});
        return spec;
    }

    spec.columns = 2;

    if (count == 2) {  // 1×2：左右各一
        spec.cells.append(GridCell{0, 0, 1, 1});
        spec.cells.append(GridCell{0, 1, 1, 1});
        return spec;
    }

    if (count == 3) {  // 左大右两小：左列跨两行，右列上下各一
        // 不用「2×2 空一格」—— 同样面积下那是最浪费的一种排法。
        spec.cells.append(GridCell{0, 0, 2, 1});
        spec.cells.append(GridCell{0, 1, 1, 1});
        spec.cells.append(GridCell{1, 1, 1, 1});
        return spec;
    }

    // 4 台（及以上兜底）：2 列瀑布 —— 每两个一行。
    // 同一个公式同时给出 2×2（count==4）与越界时不重叠的落位。
    for (int i = 0; i < count; ++i)
        spec.cells.append(GridCell{i / 2, i % 2, 1, 1});
    return spec;
}

QWidget *ViewGrid::viewAt(int index) const
{
    if (index < 0 || index >= views_.size())
        return nullptr;
    return views_[index];
}

int ViewGrid::indexOfView(QWidget *view) const
{
    return view ? views_.indexOf(view) : -1;
}

void ViewGrid::setViews(const QVector<QWidget *> &views)
{
    // 旧视窗先摘钩子**并摘出布局** —— 它们可能被复用到别处，或即将被销毁。
    // 只摘 eventFilter 不够：下面 views_ 被整体覆盖后，relayout() 遍历的是**新**列表，
    // 不在新列表里的旧视窗其布局项就无人摘除（同格双 item / 幽灵格）。
    for (QWidget *w : std::as_const(views_)) {
        if (w) {
            w->removeEventFilter(this);
            if (grid_)
                grid_->removeWidget(w);
        }
    }

    // 「占满 / 选中」记的都是**下标**，而新名单里同一个视窗的下标很可能变了
    //（中间少一格 ⇒ 它后面的全部前移）。所以先按**视窗身份**把它们记下来，
    // 覆盖 views_ 之后再找回位置 —— 只看下标的话，「删掉第 0 格」会让本来占满的
    // 第 4 格越界成「没占满」，而选中会漂到隔壁格上（用户看到的「操作目标」换了）。
    QWidget *const keepMaximized = viewAt(maximized_);
    QWidget *const keepSelected  = viewAt(selected_);

    views_ = views;

    for (QWidget *w : std::as_const(views_)) {
        if (w)
            w->installEventFilter(this);
    }

    // 归一化：按身份重映射；那一格已经不在名单里就退回默认（无占满 / 第一格）；
    // 选中退回时保留原下标并钳进范围 —— 语义是「选它旁边的那个」，
    // 比无脑跳回第 0 格更接近用户预期（被关掉的那格前后都还在）。
    maximized_ = indexOfView(keepMaximized);

    if (views_.isEmpty()) {
        selected_ = 0;
    } else {
        const int mapped = indexOfView(keepSelected);
        // 值没变时 setSelectedIndex 会自己跳过 emit，正好；
        // 真变了就发一次，让上层跟着把「操作目标」切过去。
        setSelectedIndex(mapped >= 0 ? mapped : qBound(0, selected_, views_.size() - 1));
    }

    relayout();
}

void ViewGrid::setMaximizedIndex(int index)
{
    const int normalized = (index >= 0 && index < views_.size()) ? index : -1;
    if (normalized == maximized_)
        return;

    maximized_ = normalized;
    relayout();
}

void ViewGrid::setSelectedIndex(int index)
{
    if (views_.isEmpty()) {
        selected_ = 0;
        return;
    }

    const int clamped = qBound(0, index, views_.size() - 1);
    if (clamped == selected_)
        return;

    selected_ = clamped;
    emit selectedChanged(selected_);
}

void ViewGrid::relayout()
{
    if (!grid_)
        return;

    // ① 先全部从布局摘掉。少了这一步，同一个 widget 会因为「旧位置还在」
    // 而在新布局里留下幽灵格子（错位 / 尺寸乱跳）。
    for (QWidget *w : std::as_const(views_)) {
        if (w)
            grid_->removeWidget(w);
    }

    // ② 上一套布局留下的伸缩因子归零：不清的话「上一套的 2 列伸缩」会带到
    // 1 列的布局里，把唯一一列压成半宽。
    for (int c = 0; c < grid_->columnCount(); ++c)
        grid_->setColumnStretch(c, 0);
    for (int r = 0; r < grid_->rowCount(); ++r)
        grid_->setRowStretch(r, 0);

    if (views_.isEmpty())
        return;

    const GridSpec spec = layoutForCount(views_.size());
    const int      rows = rowsOf(spec);

    // ③ 占满态：只把那一格铺满，其余 hide()（**不销毁** —— 帧照收，切回来立刻有画面）。
    if (isMaximized()) {
        QWidget *w = views_[maximized_];
        grid_->addWidget(w, 0, 0, rows, spec.columns);
        w->show();
        for (int i = 0; i < views_.size(); ++i) {
            if (i != maximized_ && views_[i])
                views_[i]->hide();
        }
        return;
    }

    // ④ 常规态：按纯函数给的落位逐一放回。
    for (int i = 0; i < views_.size(); ++i) {
        QWidget *w = views_[i];
        if (!w)
            continue;
        const GridCell &c = spec.cells[i];
        grid_->addWidget(w, c.row, c.col, c.rowSpan, c.colSpan);
        w->show();
    }

    // ⑤ 行列等分。不给伸缩的话 QGridLayout 按 sizeHint 分配 —— 几个同尺寸的
    // renderer 恰好均分，但某格 sizeHint 一变（换过控件 / 空了）就会一大一小。
    for (int c = 0; c < spec.columns; ++c)
        grid_->setColumnStretch(c, 1);
    for (int r = 0; r < rows; ++r)
        grid_->setRowStretch(r, 1);
}

bool ViewGrid::eventFilter(QObject *watched, QEvent *event)
{
    // 选中是**副作用**，绝不能吃掉事件：画面区的鼠标按键要照常走 ScreenRenderer
    // 的输入采集路径（那是真正发往远端的操作）。所以这里只记一笔，永远返回 false。
    if (event->type() == QEvent::MouseButtonPress) {
        auto     *w     = qobject_cast<QWidget *>(watched);
        const int index = w ? views_.indexOf(w) : -1;
        if (index >= 0)
            setSelectedIndex(index);
    }

    return QWidget::eventFilter(watched, event);
}

} // namespace lanrdp
