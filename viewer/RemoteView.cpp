#include "viewer/RemoteView.h"

#include "app/Glyph.h"
#include "app/Theme.h"
#include "net/ClientConnection.h"
#include "viewer/EmptyState.h"
#include "viewer/ScreenOverlayBar.h"
#include "viewer/ScreenRenderer.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QToolButton>

#include <utility>

namespace lanrdp {

namespace {

// 条上图标按钮的图标边长。条高 30 逻辑像素，18 留得出上下各 6px 的呼吸；
// 再小（16）箭头就糊了，再大（20）会把按钮撑到跟条一样高。
constexpr int kBarIconSize = 18;

// 条上按钮统一工厂。
// 用 QToolButton 而不是 QPushButton：条高只有 30 逻辑像素，QPushButton 的
// min-height（主窗口 QSS 里 26px + padding）会把它撑出条外。
// ⚠️ QToolButton 未设 icon 时会自动按「只显示文字」计算尺寸，所以 setText 可见。
QToolButton *newBarButton(QWidget *parent, const QString &text, const QString &objectName)
{
    auto *b = new QToolButton(parent);
    b->setObjectName(objectName);
    b->setText(text);
    // 条上按钮一律不进键盘焦点链 —— 焦点必须始终留给画面。
    // 否则「点一下条上的按钮」就把焦点悄悄搬走了，而占满态下 Esc 是唯一出口，
    // 它的拦截只装在画面控件上：焦点在按钮上时按 Esc 毫无反应，用户只能再去找
    // 那颗【还原】。条上全是鼠标驱动的动作按钮，不靠 Tab 到达。
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

} // namespace

RemoteView::RemoteView(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
}

RemoteView::~RemoteView() = default;   // connection_ 在此销毁（头文件里只有前向声明）

void RemoteView::buildUi()
{
    // ---- 本格的布局：只有一个孩子（画面），让它铺满 ----
    //
    // ⚠️ 这一步**不能省**（验收时抓到的真 bug）：
    // 画面是「裸子控件」时，Qt **不会**让它跟着本格 resize —— `Expanding` 的
    // sizePolicy 只在**布局**里才被兑现。少了这个布局，`ScreenRenderer` 会一直
    // 停在自己的 `setMinimumSize(320, 240)` 上：真机量出来画面只有 485 设备像素宽
    // （≈323 逻辑 px），而窗口内容区有 1662 —— 右侧七成是空白。
    // 所以「一格 = 画面铺满」这件事必须由布局来保证，不是靠 sizePolicy 声明。
    //
    // 零边距零间距：格子本身已经是画面的边框，再加边距就是白吃的像素
    //（同 ViewGrid 的处理）。
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 画面 ----
    // 本格铺满自己：格子多大，画面就多大（等比缩放与 letterbox 由 renderer 自己管）。
    renderer_ = new ScreenRenderer(this);
    root->addWidget(renderer_);

    // ---- 空态层 ----
    // 铺满画面，**先于条创建**：z 序按创建先后，条要压在它上面，
    // 否则空格里条会被空态层盖住（用户连「连接对方电脑」都点不到）。
    // 默认 hide()，显隐由 MainWindow 按这格的连接状态刷（见 refreshViewUi）。
    emptyState_ = new EmptyState(renderer_);

    // ---- 顶部悬浮条 ----
    // 条挂在**画面控件**之下（而不是本格之上）：它要浮在画面上、并跟着画面尺寸定位。
    // 「点条不会漏到远端」的两道防线在 ScreenOverlayBar 自己身上（显式 accept +
    // WA_NoMousePropagation），换个父控件也照旧成立。
    bar_ = new ScreenOverlayBar(renderer_);
    QHBoxLayout *row = bar_->actionLayout();

    // 插入顺序 = 显示顺序（同一时刻只有一批可见，由 MainWindow 按这格的连接状态定）：
    // [已连 N 个（监听中）] [占满/还原] [操作权状态] [让出操作权] [发送文件] [断开] [连接另一台]
    // 断线：[已连 N 个（监听中）] [重连] [关闭]
    // 空格：空态层盖住画面，条上只剩 [已连 N 个（监听中）] [关闭]。
    //
    // 条上除「已连 N 个」外全是**纯图标**：文字只留在悬停提示与无障碍名字里。
    // 理由是宽度 —— 一控多 2×2 时每格的条只有半个窗口宽，一排文字按钮会挤到临界；
    // 而且状态切换只换图形，不会出现「占满后文案变【还原】、按钮宽度跟着跳」那种抖动。
    //
    // 「已连 N 个」是唯一带文字的例外，放条上**最左**：几个人得一眼读到，而且它是
    // 全局状态（不随格的连接状态变化），放最左不会被右侧的按格按钮挤走。
    clientListButton_ = newBarButton(bar_, QStringLiteral("0"), QStringLiteral("clientListButton"));
    clientListButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    clientListButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    clientListButton_->setIcon(glyph::icon(glyph::Shape::Person, theme::colors().textPrimary));
    clientListButton_->setAccessibleName(QStringLiteral("已连 0 个控制端"));
    clientListButton_->setToolTip(QStringLiteral("暂无控制端连接"));
    clientListButton_->setEnabled(false);
    clientListButton_->setVisible(false);   // 未监听时隐藏，由 MainWindow 按监听状态刷
    row->addWidget(clientListButton_);

    maximizeButton_ = newBarButton(bar_, QString(), QStringLiteral("maximizeButton"));
    maximizeButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    maximizeButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    refreshMaximizeButton();
    row->addWidget(maximizeButton_);

    // 操作权状态：一颗**不可点**的图标（点它没有任何动作，所以不设 hover 反馈，
    // 样式表里把 hover / pressed 都按成透明）。为什么不用 QLabel 放位图：
    // 位图要自己按 devicePixelRatio 出图，窗口挪到另一块缩放比不同的屏上就糊了；
    // QIcon 由 Qt 按当前缩放比重新出图，跨屏不掉画质。
    tokenStatusButton_ = newBarButton(bar_, QString(), QStringLiteral("tokenStatusButton"));
    tokenStatusButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tokenStatusButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    refreshTokenStatus();
    row->addWidget(tokenStatusButton_);

    releaseTokenButton_ = newBarButton(bar_, QString(), QStringLiteral("releaseTokenButton"));
    releaseTokenButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    releaseTokenButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    releaseTokenButton_->setIcon(glyph::icon(glyph::Shape::Unlock, theme::colors().textPrimary));
    releaseTokenButton_->setAccessibleName(QStringLiteral("让出操作权"));
    releaseTokenButton_->setToolTip(QStringLiteral(
        "让出当前操作权，允许其他控制端操作对方的电脑。"));
    releaseTokenButton_->setEnabled(false);   // 手上没有操作权时无从让出
    row->addWidget(releaseTokenButton_);

    // 【屏幕】—— 只在被控端有多块屏时出现（单屏用户界面零变化）。
    // 图形用现成的显示器轮廓：它只在此处出现，不需要新画一对「两块屏」的图形，
    // 少一份要跟着 DPR 调的几何就少一处出错的地方。
    screenButton_ = newBarButton(bar_, QString(), QStringLiteral("screenButton"));
    screenButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    screenButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    screenButton_->setIcon(glyph::icon(glyph::Shape::Monitor, theme::colors().textPrimary));
    screenButton_->setAccessibleName(QStringLiteral("选择屏幕"));
    screenButton_->setToolTip(QStringLiteral("选择要看的屏幕"));
    screenButton_->setVisible(false);
    row->addWidget(screenButton_);

    sendFileButton_ = newBarButton(bar_, QString(), QStringLiteral("sendFileButton"));
    sendFileButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    sendFileButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    sendFileButton_->setIcon(glyph::icon(glyph::Shape::SendFile, theme::colors().textPrimary));
    sendFileButton_->setAccessibleName(QStringLiteral("发送文件"));
    sendFileButton_->setToolTip(QStringLiteral(
        "向对方发送文件，文件将保存到对方的接收目录，完成后自动校验完整性。\n"
        "也可以将文件拖入画面发送。"));
    row->addWidget(sendFileButton_);

    disconnectButton_ = newBarButton(bar_, QString(), QStringLiteral("disconnectButton"));
    disconnectButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    disconnectButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    // 危险动作用告警色 —— 文字按钮时靠样式表的 color，换成图标后颜色得落在图形上。
    disconnectButton_->setIcon(glyph::icon(glyph::Shape::Disconnect, theme::colors().danger));
    disconnectButton_->setAccessibleName(QStringLiteral("断开"));
    disconnectButton_->setToolTip(QStringLiteral(
        "断开此连接：画面停止刷新，当前操作权将自动释放。"));
    disconnectButton_->setEnabled(false);
    row->addWidget(disconnectButton_);

    // 断线格的两个出口。**只在断线时出现**（显隐由 MainWindow 按状态刷）——
    // 活着的时候摆一个【关闭】在条上，等于在诱导用户误点掉一格。
    retryButton_ = newBarButton(bar_, QString(), QStringLiteral("retryButton"));
    retryButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    retryButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    retryButton_->setIcon(glyph::icon(glyph::Shape::Reconnect, theme::colors().textPrimary));
    retryButton_->setAccessibleName(QStringLiteral("重连"));
    retryButton_->setToolTip(QStringLiteral(
        "使用上次的地址与口令立即重新连接，不必等待自动重连。"));
    row->addWidget(retryButton_);

    closeCellButton_ = newBarButton(bar_, QString(), QStringLiteral("closeCellButton"));
    closeCellButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    closeCellButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    closeCellButton_->setIcon(glyph::icon(glyph::Shape::CloseCell, theme::colors().textPrimary));
    closeCellButton_->setAccessibleName(QStringLiteral("关闭这一格"));
    closeCellButton_->setToolTip(QStringLiteral(
        "关闭此画面，其余画面不受影响。\n"
        "仅剩一个画面时，将清空画面并回到初始状态。"));
    row->addWidget(closeCellButton_);

    connectButton_ = newBarButton(bar_, QString(), QStringLiteral("connectButton"));
    connectButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connectButton_->setIconSize(QSize(kBarIconSize, kBarIconSize));
    connectButton_->setIcon(glyph::icon(glyph::Shape::NewConnection, theme::colors().textPrimary));
    connectButton_->setAccessibleName(QStringLiteral("连接另一台"));
    connectButton_->setToolTip(QStringLiteral(
        "新增一个画面，并在其中选择连接远程电脑或允许远程电脑连接本机。\n"
        "最多同时 4 个画面。"));
    row->addWidget(connectButton_);

    // ---- 「点这一格 = 选中这一格」----
    // 过滤器装在画面控件与条上：它们的点击才算「操作这一格」。
    // ⚠️ 条上**按钮**的点击到不了这里（Qt 的命中测试会先把事件交给按钮），
    // 那是好事 —— 按钮各自知道该作用于哪一格，本来就不需要先切选中。
    renderer_->installEventFilter(this);
    bar_->installEventFilter(this);
}

void RemoteView::setConnection(std::unique_ptr<ClientConnection> conn)
{
    connection_ = std::move(conn);
}

std::unique_ptr<ClientConnection> RemoteView::takeConnection()
{
    return std::move(connection_);
}

void RemoteView::setSelected(bool on)
{
    if (selected_ == on)
        return;
    selected_ = on;
    if (bar_)
        bar_->setSelected(on);
}

// 占满态镜像。只做显示（图标 + 提示语），不改布局 —— 布局归 ViewGrid。
void RemoteView::setMaximized(bool on)
{
    if (maximized_ == on)
        return;
    maximized_ = on;
    refreshMaximizeButton();
}

// 状态跟着图标走：占满后箭头转成朝内收（这颗按钮现在是出口），
// 提示语同步换成出口的说明 —— 按钮上没有文字，提示是唯一能说清的地方。
void RemoteView::refreshMaximizeButton()
{
    if (!maximizeButton_)
        return;

    const bool on = maximized_;

    maximizeButton_->setIcon(glyph::icon(
        on ? glyph::Shape::Restore : glyph::Shape::Maximize, theme::colors().textPrimary));
    maximizeButton_->setAccessibleName(on ? QStringLiteral("还原") : QStringLiteral("占满"));
    maximizeButton_->setToolTip(on
        ? QStringLiteral("退出占满，恢复多画面布局。\n（占满时按 Esc 等效）")
        : QStringLiteral(
              "将此画面铺满窗口区域；其余画面被隐藏但连接不断，\n"
              "还原后立即恢复显示。占满时按 Esc 也可还原。"));
}

// 操作权状态：实心指针 = 手上握着操作权，空心指针 = 还没拿到。
// 三档只差「填充」和「颜色深浅」两件事 —— 图形始终是同一个指针，
// 用户不会把它读成三个不同的东西。
void RemoteView::setTokenState(TokenState state)
{
    if (tokenState_ == state)
        return;
    tokenState_ = state;
    refreshTokenStatus();
}

void RemoteView::refreshTokenStatus()
{
    if (!tokenStatusButton_)
        return;

    const theme::Colors &c = theme::colors();

    glyph::Shape shape = glyph::Shape::Pointer;
    QColor       color = c.textPrimary;
    QString      what;   // 状态本身

    switch (tokenState_) {
    case TokenState::Held:
        what = QStringLiteral("已获得");
        break;
    case TokenState::Waiting:
        shape = glyph::Shape::PointerIdle;
        color = c.textSecondary;
        what  = QStringLiteral("等待中");
        break;
    case TokenState::NotConnected:
        shape = glyph::Shape::PointerIdle;
        color = c.textMuted;
        what  = QStringLiteral("未连接");
        break;
    }

    tokenStatusButton_->setIcon(glyph::icon(shape, color));
    tokenStatusButton_->setAccessibleName(QStringLiteral("操作权：%1").arg(what));

    // 提示语要把「这意味着什么」和「拿不到时怎么办」都说清 —— 按钮上没有文字，
    // 也只显示一个图标，这里是唯一的解释处。
    QString tip = QStringLiteral("操作权：%1\n").arg(what);
    switch (tokenState_) {
    case TokenState::Held:
        tip += QStringLiteral("当前已获得操作权，可控制对方电脑。");
        break;
    case TokenState::Waiting:
        tip += QStringLiteral("操作权由对方掌握。点击画面可再次申请；\n"
                              "也可能是对方已锁定键鼠输入。");
        break;
    case TokenState::NotConnected:
        tip += QStringLiteral("尚未建立连接，无法控制对方电脑。");
        break;
    }
    tokenStatusButton_->setToolTip(tip);
}

void RemoteView::setTitle(const QString &text)
{
    if (bar_)
        bar_->setTitle(text);
}

QString RemoteView::title() const
{
    return bar_ ? bar_->title() : QString();
}

// 断线态。只做显示（灰显 + 说明文字），画面本身**不清** —— 保留最后一帧
// 正是这个状态存在的理由。两个出口按钮的显隐由 MainWindow 统一刷（它才知道状态）。
void RemoteView::setDisconnected(bool on, const QString &staleText)
{
    if (!staleText.isEmpty())
        renderer_->setStaleText(staleText);
    if (disconnected_ == on)
        return;
    disconnected_ = on;
    renderer_->setStale(on);
}

bool RemoteView::eventFilter(QObject *watched, QEvent *event)
{
    // ---- 占满态下把 Esc 本地化 ----
    // 设计上 Esc 平时属于远端（Vim / 对话框 / PPT 都要用），**只有占满态**才是
    // 「退出全屏」的本地手势 —— 此时画面铺满，鼠标可能够不到别的出口。
    //
    // ⚠️ 这一条与下面的 activated **相反**：它必须**消费**事件（return true），
    // 否则远端会白收一颗 Esc（`viewer/KeyMap.cpp` 把 Escape 映射成 0x1B，
    // 而且 keyChanged 无条件转发，连"未持令牌"都会顺手发一帧令牌申请）。
    // 之所以能拦截，是因为过滤器在**事件交给 renderer 之前**回调 ——
    // 不依赖 renderer 的 inputEnabled 状态，也不依赖它 accept 还是 ignore。
    if (watched == renderer_) {
        if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (maximized_ && ke->key() == Qt::Key_Escape && !ke->isAutoRepeat()) {
                escConsumed_ = true;
                emit restoreRequested(this);
                return true;
            }
            // 非占满态的 Esc 是要**转发给远端**的正常按键。若上一颗占满态的 Esc 抬起
            // 因焦点被抢 / 该格被隐藏而从未送达本控件，escConsumed_ 会残留在 true ——
            // 那会让这一颗的「抬起」被误吞，远端只收到按下（卡键）。这里顺手作废残标记。
            if (!maximized_ && ke->key() == Qt::Key_Escape)
                escConsumed_ = false;
        } else if (event->type() == QEvent::KeyRelease && escConsumed_) {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->key() == Qt::Key_Escape && !ke->isAutoRepeat()) {
                escConsumed_ = false;
                return true;   // 抬起也一起吃掉：别让远端收到「只有抬起」的 Esc
            }
        }
    }

    // 旁观点击：只上报「用户在操作这一格」，**不消费**事件 ——
    // 画面区的左键/右键必须照常走 ScreenRenderer 的输入采集路径（那是真正发往远端的
    // 操作），所以这里永远返回 false。
    //
    // 这是**唯一**的选中入口：ViewGrid 曾有一段「自己装 eventFilter 旁听点击」的
    // 重复实现，但 renderer / 条 / 空态都覆盖整格并 accept，那段代码实际不可达
    // （S6，2026-10-25 已删）。别再加回去。
    //
    // 过滤器装在子控件上（而不是靠事件冒泡上来的父控件）：Qt 的 installEventFilter
    // 在**事件交给目标本身之前**就回调，所以哪怕 ScreenRenderer 把事件 accept 掉了，
    // 这里照样收得到 —— 不依赖「谁有没有 ignore」。
    if ((watched == renderer_ || watched == bar_) && event->type() == QEvent::MouseButtonPress)
        emit activated(this);

    return QWidget::eventFilter(watched, event);
}

} // namespace lanrdp
