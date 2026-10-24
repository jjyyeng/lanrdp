#include "viewer/ScreenOverlayBar.h"

#include "app/Theme.h"

#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QWheelEvent>

namespace lanrdp {

namespace {

// 条高（逻辑像素）。取 30 是被两头夹出来的：
// · 再矮：条内按钮会被裁（Qt 默认控件的 sizeHint 就接近这个数）；
// · 再高：明显吃掉远端窗口的标题栏 —— 远端窗口的「最大化 / 关闭」按钮会点不到。
constexpr int kBarHeight = 30;

} // namespace

int ScreenOverlayBar::barHeight()
{
    return kBarHeight;
}

QRect ScreenOverlayBar::geometryFor(const QSize &parentSize)
{
    return QRect(0, 0, qMax(0, parentSize.width()), kBarHeight);
}

ScreenOverlayBar::ScreenOverlayBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("overlayBar"));
    setFixedHeight(kBarHeight);
    setAutoFillBackground(false);   // 自己画（半透明），不走 palette

    // 「点条 = 点远端」的第二道保险（第一道是下面那组事件覆写里显式 accept）。
    // 单靠这个属性也能拦住鼠标冒泡，但键盘事件不受它管，所以两者都写。
    setAttribute(Qt::WA_NoMousePropagation, true);

    // 紧凑化：主窗口那份 QSS 给按钮定了 min-height 26px，条里放不下。
    // 条内统一用 QToolButton 并单独收紧（工具按钮本来就该扁平紧凑）。
    // 注意：QToolButton 没设 icon 时会自动按「只显示文字」算尺寸，所以 setText 可见。
    //
    // 配色不写在这里 —— 条内的样式与全局主题共用同一份色板，见 app/Theme.cpp。
    // 条压在画面上，无论下面是白色网页还是深色壁纸都得站得住，所以它是深底浅字。
    setStyleSheet(lanrdp::theme::overlayBarStyleSheet());

    auto *root = new QHBoxLayout(this);
    // 左边留 16：多出来的 8px 是给状态点的（画在 x≈9，不与标题重叠）。
    root->setContentsMargins(16, 0, 8, 0);
    root->setSpacing(6);

    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("overlayTitle"));
    root->addWidget(title_);

    actions_ = new QHBoxLayout();
    actions_->setContentsMargins(0, 0, 0, 0);
    actions_->setSpacing(6);
    root->addLayout(actions_);

    root->addStretch(1);

    // 跟随**父控件**尺寸变化。做成 eventFilter 而不是让父控件回调本类，
    // 是为了「谁拥有画面谁不管条」—— 每个视窗一份，父控件不必知道有它。
    if (parent)
        parent->installEventFilter(this);

    relayout();
}

void ScreenOverlayBar::setTitle(const QString &text)
{
    if (title_)
        title_->setText(text);
}

QString ScreenOverlayBar::title() const
{
    return title_ ? title_->text() : QString();
}

void ScreenOverlayBar::setSelected(bool on)
{
    if (selected_ == on)
        return;
    selected_ = on;
    update();   // 只重画，不动几何 —— 见头文件的说明
}

void ScreenOverlayBar::setStatus(Status status)
{
    if (status_ == status)
        return;
    status_ = status;
    update();
}

void ScreenOverlayBar::relayout()
{
    QWidget *parent = parentWidget();
    if (!parent)
        return;

    const QRect target = geometryFor(parent->size());
    if (geometry() != target)
        setGeometry(target);
    raise();   // 必须压在画面之上（resize 后重保一次，别被后来者盖住）
}

bool ScreenOverlayBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        relayout();
    return QWidget::eventFilter(watched, event);
}

// 条上右键什么都不做，但**必须吃掉**该事件。
//
// 不是「保留一个空槽」凑数：Qt 里未处理的上下文菜单事件会冒泡给父控件，而条的父控件
// 正是画面控件 —— 不吞就会在远端弹出一个右键菜单（与下面「点条 = 点远端」同一类问题）。
void ScreenOverlayBar::contextMenuEvent(QContextMenuEvent *event)
{
    event->accept();
}

// ---------------------------------------------------------------------------
// 「点条 = 点远端」的防线
//
// 下面这组覆写每个都只有一句 `event->accept()`，看着像废话，其实是**必需的**：
// QWidget 对鼠标 / 键盘事件的默认实现一律 `ignore()`，而 Qt 对**未处理**的这两类
// 事件会**向上传播给父控件** —— 条的父控件正是画面控件，于是：
// · 点条上的空白处 -> 画面控件收到 mousePress -> 转发给远端（等于点了一下远端）
// · 按 Esc / 任意键 -> 画面控件收到 keyPress -> 转发给远端（还顺手敲一次令牌门）
//
// 这不是理论风险：`tests/test_mainwindow_ui.cpp` 的
// `overlayBarDoesNotForwardInputToRemote` 第一版跑出来，5 次模拟点击**全部**
// 冒泡到了画面控件。只把条做成独立子控件是**不够的**，还得让它把事件吃掉。
//
// 条上按钮（子控件）的点击不受影响：Qt 的命中测试会先把事件交给按钮，
// 根本轮不到条自己。
// ---------------------------------------------------------------------------
void ScreenOverlayBar::mousePressEvent(QMouseEvent *event)       { event->accept(); }
void ScreenOverlayBar::mouseReleaseEvent(QMouseEvent *event)     { event->accept(); }
void ScreenOverlayBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    emit doubleClicked();   // 落在标题区（按钮是子控件，它们的双击到不了这里）
}
void ScreenOverlayBar::mouseMoveEvent(QMouseEvent *event)        { event->accept(); }
void ScreenOverlayBar::wheelEvent(QWheelEvent *event)            { event->accept(); }
void ScreenOverlayBar::keyPressEvent(QKeyEvent *event)           { event->accept(); }
void ScreenOverlayBar::keyReleaseEvent(QKeyEvent *event)         { event->accept(); }

void ScreenOverlayBar::paintEvent(QPaintEvent *)
{
    const lanrdp::theme::Colors &c = lanrdp::theme::colors();

    QPainter p(this);

    // 92% 不透明：留一点画面透出来，让人看得出「条浮在画面上」；
    // 再低就压不住白色远端窗口，文字会跟着背景一起发飘。
    QColor background = selected_ ? c.overlayBarSelected : c.overlayBar;
    background.setAlpha(selected_ ? 242 : 235);
    p.fillRect(rect(), background);

    // 选中态：左侧一小段强调色竖条。
    // 不用「整条底边画一条 2px 横线」—— 条宽能到 1600+ 像素，一条贯穿全宽的
    // 饱和蓝线在深色画面上比画面本身还抢眼，看起来像个装饰条而不是选中标记。
    if (selected_)
        p.fillRect(0, (height() - 14) / 2, 3, 14, c.accent);

    // 底边一条淡分隔线：条与画面之间需要一个终止符，否则它看起来像画面内容。
    p.setPen(c.border);
    p.drawLine(0, height() - 1, width(), height() - 1);

    // 状态点（7px）。画在标题左侧那 8px 留白里，与选中竖条（x=0..3）不重叠。
    QColor dot;
    switch (status_) {
    case Status::Connected:    dot = c.success;    break;
    case Status::Reconnecting: dot = c.warning;    break;
    case Status::Disconnected: dot = c.danger;     break;
    case Status::Idle:         dot = c.textMuted;  break;
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    p.drawEllipse(QPointF(9.0, height() / 2.0), 3.5, 3.5);
}

} // namespace lanrdp
