// 多视窗网格（ViewGrid）的契约测试。
//
// 分两层：
// ① 纯函数 `layoutForCount()` —— 1×1 / 1×2 / 左大右两小 / 2×2 四条规则的落位。
// 这层完全脱离 UI（不造 widget），是布局规则唯一的权威断言。
// ② 宿主行为 —— setViews 重排、占满 / 还原、选中。
//
// ⚠️ 显隐一律用 `isHidden()` 判断，**不要**用 `isVisible()`：
// 父窗口没 show() 时，整棵子树的 isVisible() 都是 false（祖先不可见会连坐），
// 那样测的就不是「我有没有把它显隐掉」。这条在悬浮条用例上踩过。

#include "viewer/ViewGrid.h"

#include <QApplication>
#include <QByteArray>
#include <QGridLayout>
#include <QMouseEvent>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QWidget>

using namespace lanrdp;

namespace {

// 把所有格按「被覆盖到的单元格」展开去重 —— 拿它同时验「无重叠」与「无空洞」：
// 去重后的数量 == 网格总格数 ⇒ 既不重叠（重叠会让去重后变少）也不留空（留空也变少）。
int coveredCells(const GridSpec &spec)
{
    QSet<QString> cells;
    for (const GridCell &c : spec.cells) {
        for (int r = c.row; r < c.row + c.rowSpan; ++r) {
            for (int col = c.col; col < c.col + c.colSpan; ++col)
                cells.insert(QStringLiteral("%1,%2").arg(r).arg(col));
        }
    }
    return cells.size();
}

int gridRows(const GridSpec &spec)
{
    int rows = 1;
    for (const GridCell &c : spec.cells)
        rows = qMax(rows, c.row + c.rowSpan);
    return rows;
}

int gridCellTotal(const GridSpec &spec)
{
    return gridRows(spec) * spec.columns;
}

// 取出某控件在网格里的落位。
// ⚠️ Qt6 的 QGridLayout **没有** `indexOf(w, &row, &col, …)` 这个重载了（Qt5 有），
// 只能先 `indexOf(w)` 拿索引，再 `getItemPosition(index, …)` 取落位。
bool cellOf(QGridLayout *layout, QWidget *w, int *row, int *col, int *rs, int *cs)
{
    const int index = layout->indexOf(w);
    if (index < 0)
        return false;
    layout->getItemPosition(index, row, col, rs, cs);
    return true;
}

// 造一批占位控件（ViewGrid 不认识 ScreenRenderer，只当它是普通 QWidget）。
QVector<QWidget *> makeWidgets(int n, QWidget *parent)
{
    QVector<QWidget *> out;
    for (int i = 0; i < n; ++i)
        out.append(new QWidget(parent));
    return out;
}

} // namespace

class TestViewGrid : public QObject
{
    Q_OBJECT

private slots:
    // -----------------------------------------------------------------------
    // ① 纯函数：布局规则
    // -----------------------------------------------------------------------

    // 对 1..4 每一档：格数对得上、**无重叠且铺满**（这四档都是设计好的满铺布局）。
    void layoutForCountTilesEveryCell_data()
    {
        QTest::addColumn<int>("count");
        for (int n = 1; n <= 4; ++n) {
            const QByteArray name = QByteArray::number(n);
            QTest::newRow(name.constData()) << n;
        }
    }

    void layoutForCountTilesEveryCell()
    {
        QFETCH(int, count);
        const GridSpec spec = ViewGrid::layoutForCount(count);

        QCOMPARE(spec.cells.size(), count);
        QVERIFY2(spec.columns >= 1, "列数必须为正");
        // 去重展开 == 网格总格数 ⇒ 既不重叠（重叠会让去重后变少）也不留空
        QCOMPARE(coveredCells(spec), gridCellTotal(spec));
    }

    // 0 个视窗（防御性分支）与负数：给空落位，绝不给重叠的东西。
    void layoutForNoViewsIsEmpty()
    {
        const GridSpec zero = ViewGrid::layoutForCount(0);
        QCOMPARE(zero.cells.size(), 0);
        QCOMPARE(zero.columns, 1);

        const GridSpec negative = ViewGrid::layoutForCount(-3);
        QCOMPARE(negative.cells.size(), 0);
    }

    void oneViewFillsTheWholeGrid()
    {
        const GridSpec spec = ViewGrid::layoutForCount(1);
        QCOMPARE(spec.columns, 1);
        QCOMPARE(spec.cells.size(), 1);
        QCOMPARE(spec.cells[0].row, 0);
        QCOMPARE(spec.cells[0].col, 0);
        QCOMPARE(spec.cells[0].rowSpan, 1);
        QCOMPARE(spec.cells[0].colSpan, 1);
    }

    void twoViewsSitSideBySide()
    {
        const GridSpec spec = ViewGrid::layoutForCount(2);
        QCOMPARE(spec.columns, 2);
        QCOMPARE(spec.cells.size(), 2);
        for (const GridCell &c : spec.cells) {
            QCOMPARE(c.row, 0);          // 同一行
            QCOMPARE(c.rowSpan, 1);
            QCOMPARE(c.colSpan, 1);
        }
        QCOMPARE(spec.cells[0].col, 0);
        QCOMPARE(spec.cells[1].col, 1);
    }

    // 3 台时"左大右两小"：左列一格跨两行，右列上下各一格。
    // 刻意**不用**"2×2 空一格" —— 同样面积下那是最浪费的一种排法。
    void threeViewsPutOneBigOnTheLeft()
    {
        const GridSpec spec = ViewGrid::layoutForCount(3);
        QCOMPARE(spec.columns, 2);
        QCOMPARE(spec.cells.size(), 3);

        const GridCell big = spec.cells[0];
        QCOMPARE(big.col, 0);
        QCOMPARE(big.row, 0);
        QCOMPARE(big.rowSpan, 2);
        QCOMPARE(big.colSpan, 1);

        // 右列上下两小格
        QCOMPARE(spec.cells[1].col, 1);
        QCOMPARE(spec.cells[1].row, 0);
        QCOMPARE(spec.cells[2].col, 1);
        QCOMPARE(spec.cells[2].row, 1);
        QCOMPARE(spec.cells[1].rowSpan, 1);
        QCOMPARE(spec.cells[2].rowSpan, 1);
    }

    void fourViewsFormAGridTwoByTwo()
    {
        const GridSpec spec = ViewGrid::layoutForCount(4);
        QCOMPARE(spec.columns, 2);
        QCOMPARE(spec.cells.size(), 4);
        QCOMPARE(gridRows(spec), 2);

        const QVector<QPair<int, int>> expected = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(spec.cells[i].row, expected[i].first);
            QCOMPARE(spec.cells[i].col, expected[i].second);
        }
    }

    // 上限是 4 台，但纯函数在越界时也不该给出**重叠**的落位（那会让 QGridLayout
    // 出幽灵格子）。兜底 = 「2 列瀑布」：继续往下排行号递增。
    // ⚠️ 奇数格在瀑布里必然留一个空位 —— 这里只要求「不重叠」，
    // 「铺满」是 1..4 那四档才有的性质（由 layoutForCountTilesEveryCell 守着）。
    void beyondFourFallsBackToWaterfall()
    {
        const GridSpec spec = ViewGrid::layoutForCount(5);
        QCOMPARE(spec.cells.size(), 5);
        QCOMPARE(spec.columns, 2);
        QCOMPARE(coveredCells(spec), 5);   // 去重后仍是 5 格 ⇒ 无重叠
        QCOMPARE(gridRows(spec), 3);
        QCOMPARE(spec.cells[4].row, 2);
        QCOMPARE(spec.cells[4].col, 0);
    }

    // -----------------------------------------------------------------------
    // ② 宿主行为
    // -----------------------------------------------------------------------

    void setViewsPlacesEveryWidgetInTheLayout()
    {
        ViewGrid grid;
        const QVector<QWidget *> ws = makeWidgets(3, &grid);
        grid.setViews(ws);

        QCOMPARE(grid.viewCount(), 3);
        auto *layout = qobject_cast<QGridLayout *>(grid.layout());
        QVERIFY2(layout, "ViewGrid 内部应当是一个 QGridLayout");

        // 每一格都必须**恰好**在布局里出现一次（重复 addWidget 是未定义行为）
        for (QWidget *w : ws) {
            int row = -1, col = -1, rs = -1, cs = -1;
            QVERIFY2(cellOf(layout, w, &row, &col, &rs, &cs),
                     "每个视窗都应当在网格里有位置");
        }
        // 3 格 = 左大右两小
        int row = -1, col = -1, rs = -1, cs = -1;
        QVERIFY(cellOf(layout, ws[0], &row, &col, &rs, &cs));
        QCOMPARE(row, 0);
        QCOMPARE(col, 0);
        QCOMPARE(rs, 2);   // 左边那格跨两行
    }

    void maximizingOneCellHidesTheOthers()
    {
        ViewGrid grid;
        const QVector<QWidget *> ws = makeWidgets(4, &grid);
        grid.setViews(ws);

        grid.setMaximizedIndex(2);

        QVERIFY(grid.isMaximized());
        QCOMPARE(grid.maximizedIndex(), 2);
        QVERIFY2(!ws[2]->isHidden(), "占满的那一格必须可见");
        for (int i = 0; i < 4; ++i) {
            if (i != 2)
                QVERIFY2(ws[i]->isHidden(), "其余格子必须隐藏（但连接不断、帧照收）");
        }

        // 占满的那一格要铺满整个网格（跨满行与列）
        auto *layout = qobject_cast<QGridLayout *>(grid.layout());
        int row = -1, col = -1, rs = -1, cs = -1;
        QVERIFY(cellOf(layout, ws[2], &row, &col, &rs, &cs));
        QCOMPARE(row, 0);
        QCOMPARE(col, 0);
        QCOMPARE(rs, 2);
        QCOMPARE(cs, 2);
    }

    // 「占满」只是不显示，**不能**把别的视窗从对象树里摘掉 ——
    // 它们的连接还在收帧，切回来要立刻有画面。
    void maximizingKeepsHiddenViewsAlive()
    {
        ViewGrid grid;
        const QVector<QWidget *> ws = makeWidgets(3, &grid);
        grid.setViews(ws);
        grid.setMaximizedIndex(0);

        for (QWidget *w : ws) {
            QVERIFY2(w->parentWidget() == &grid, "隐藏 ≠ 销毁，视窗必须还挂在网格下");
        }
    }

    void restoringBringsEveryoneBack()
    {
        ViewGrid grid;
        const QVector<QWidget *> ws = makeWidgets(4, &grid);
        grid.setViews(ws);
        grid.setMaximizedIndex(1);
        grid.setMaximizedIndex(-1);

        QVERIFY(!grid.isMaximized());
        for (QWidget *w : ws)
            QVERIFY2(!w->isHidden(), "还原后每一格都该回到画面");
    }

    void selectedIndexClampsAndSignals()
    {
        ViewGrid grid;
        grid.setViews(makeWidgets(3, &grid));
        QCOMPARE(grid.selectedIndex(), 0);   // 默认选第 0 格

        QSignalSpy spy(&grid, &ViewGrid::selectedChanged);

        grid.setSelectedIndex(2);
        QCOMPARE(grid.selectedIndex(), 2);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(0).toInt(), 2);

        // 同一格重复设置不该再发信号（避免上层被无意义地刷）
        grid.setSelectedIndex(2);
        QCOMPARE(spy.count(), 1);

        grid.setSelectedIndex(99);           // 越界 -> 钳到最后一格
        QCOMPARE(grid.selectedIndex(), 2);
        grid.setSelectedIndex(-5);           // 越界 -> 钳到第一格
        QCOMPARE(grid.selectedIndex(), 0);
    }

    // 视窗变少时（断开一台 / 切回被控模式），越界的占满与选中索引必须归一化，
    // 否则下一次 relayout 会读到不存在的格子。
    void shrinkingViewsNormalizesMaximizedAndSelection()
    {
        ViewGrid grid;
        const QVector<QWidget *> four = makeWidgets(4, &grid);
        grid.setViews(four);
        grid.setMaximizedIndex(3);
        grid.setSelectedIndex(3);

        const QVector<QWidget *> two = {four[0], four[1]};
        grid.setViews(two);

        QCOMPARE(grid.viewCount(), 2);
        QVERIFY2(!grid.isMaximized(), "视窗少到装不下原来的占满格时应当自动还原");
        QCOMPARE(grid.selectedIndex(), 1);   // 钳到最后一格

        for (QWidget *w : two)
            if (w)
                QVERIFY2(!w->isHidden(), "重排后剩下的视窗都该显示");

        // 被摘出列表的两格必须**彻底脱离布局** —— 只把 views_ 换掉不摘布局项的话，
        // 它们会留在 QGridLayout 里变成幽灵格（同格双 item / 错位）。
        auto *layout = qobject_cast<QGridLayout *>(grid.layout());
        QVERIFY2(layout, "ViewGrid 内部应当是一个 QGridLayout");
        QVERIFY2(layout->indexOf(four[2]) < 0, "第 3 格应已脱离布局");
        QVERIFY2(layout->indexOf(four[3]) < 0, "第 4 格应已脱离布局");
    }

    // 名单中间少一格 ⇒ 它后面的视窗**全部前移**。占满 / 选中记的是下标，
    // 所以重排必须按**视窗身份**迁移：只看下标的话，本来占满的第 4 格会越界成
    // 「没占满」，选中的第 3 格会漂到第 4 格上（用户看到的「操作目标」变了）。
    void shrinkingViewsKeepsMaximizedAndSelectionOnTheSameWidget()
    {
        ViewGrid grid;
        const QVector<QWidget *> four = makeWidgets(4, &grid);
        grid.setViews(four);
        grid.setMaximizedIndex(3);   // 第 4 格占满
        grid.setSelectedIndex(2);    // 选中第 3 格

        // 摘掉**第 2 格**：第 3/4 格各前移一位（占满格 3->2、选中格 2->1）
        const QVector<QWidget *> three = {four[0], four[2], four[3]};
        grid.setViews(three);

        QCOMPARE(grid.viewCount(), 3);
        QVERIFY2(grid.isMaximized(), "占满的那一格仍在名单里，不该被当成越界而还原");
        QCOMPARE(grid.maximizedIndex(), 2);
        QCOMPARE(grid.viewAt(grid.maximizedIndex()), four[3]);
        QVERIFY2(!four[3]->isHidden(), "占满的还是原来那一格");

        QCOMPARE(grid.selectedIndex(), 1);
        QCOMPARE(grid.viewAt(grid.selectedIndex()), four[2]);

        // 反向：加一格也不该让这两项漂走（新格插在末尾，旧下标仍指旧格）
        const QVector<QWidget *> back = {four[0], four[2], four[3], four[1]};
        grid.setViews(back);
        QCOMPARE(grid.maximizedIndex(), 2);
        QCOMPARE(grid.viewAt(grid.maximizedIndex()), four[3]);
        QCOMPARE(grid.selectedIndex(), 1);
        QCOMPARE(grid.viewAt(grid.selectedIndex()), four[2]);
    }
};

QTEST_MAIN(TestViewGrid)
#include "test_viewgrid.moc"
