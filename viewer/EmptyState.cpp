#include "viewer/EmptyState.h"

#include "ui/Glyph.h"
#include "ui/Theme.h"

#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace lanrdp {

namespace {

constexpr int kIconSize   = 68;   // 空态正中那个大图标
constexpr int kButtonIcon = 20;   // 两个入口按钮上的小图标（再小箭头就糊了）

} // namespace

EmptyState::EmptyState(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("emptyState"));

    // 与条同一道保险：不靠"父控件此刻不会转发事件"的假设。
    setAttribute(Qt::WA_NoMousePropagation, true);

    // 背景保持透明 —— 画面控件已经填了 canvas 色。两层各自填色时，
    // 一旦哪天两处色值不同步，就会露出一块"补丁"。
    setAutoFillBackground(false);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(0);

    root->addStretch(1);

    icon_ = new QLabel(this);
    icon_->setObjectName(QStringLiteral("emptyIcon"));
    updateIcon();
    icon_->setAlignment(Qt::AlignCenter);
    root->addWidget(icon_, 0, Qt::AlignHCenter);

    root->addSpacing(14);

    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("emptyTitle"));
    title_->setAlignment(Qt::AlignCenter);
    root->addWidget(title_);

    root->addSpacing(6);

    subtitle_ = new QLabel(this);
    subtitle_->setObjectName(QStringLiteral("emptySubtitle"));
    subtitle_->setAlignment(Qt::AlignCenter);
    root->addWidget(subtitle_);

    root->addSpacing(20);

    auto *buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(10);

    connectButton_ = new QPushButton(this);
    connectButton_->setObjectName(QStringLiteral("emptyPrimary"));
    connectButton_->setCursor(Qt::PointingHandCursor);
    connectButton_->setIconSize(QSize(kButtonIcon, kButtonIcon));
    connect(connectButton_, &QPushButton::clicked, this, &EmptyState::connectRequested);

    hostButton_ = new QPushButton(this);
    hostButton_->setObjectName(QStringLiteral("emptySecondary"));
    hostButton_->setCursor(Qt::PointingHandCursor);
    hostButton_->setIconSize(QSize(kButtonIcon, kButtonIcon));
    connect(hostButton_, &QPushButton::clicked, this, &EmptyState::hostRequested);

    buttons->addStretch(1);
    buttons->addWidget(connectButton_);
    buttons->addWidget(hostButton_);
    buttons->addStretch(1);
    root->addLayout(buttons);

    root->addStretch(1);

    // 样式与三张图标（中央大图 + 两个入口）都在这里落地 —— 它们全都依赖当前色板，
    // 所以只有一份代码，换主题时重跑同一个函数即可。
    applyTheme();

    if (parent) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
    }

    retranslate();   // 全部文案的唯一来源（构造与语言切换各走一次）
    hide();   // 显隐由 MainWindow 按模式统一刷
}

void EmptyState::applyTheme()
{
    setStyleSheet(theme::emptyStateStyleSheet());

    // 图标颜色跟文字走：主按钮的文字是 onAccent，次按钮是 textSecondary。
    // 两个图标都指向屏幕 —— 一个箭头朝外（我去连别人），一个朝内（别人连进来）。
    if (connectButton_)
        connectButton_->setIcon(glyph::icon(glyph::Shape::MonitorOut, theme::colors().onAccent));

    updateIcon();
    applyHostButton();   // 它自己也带图形（允许 / 禁止），颜色同样取自当前色板
}

// 这一屏没有随状态变化的长文案，只有「按钮该说允许还是禁止」那一处，
// 交给 applyHostButton() 按 hostListening_ 现算。
void EmptyState::retranslate()
{
    if (title_)
        title_->setText(tr("尚未连接"));
    if (subtitle_)
        subtitle_->setText(tr("连接远程电脑，或允许远程电脑连接本机"));
    if (connectButton_) {
        connectButton_->setText(tr("连接远程电脑"));
        connectButton_->setToolTip(tr(
            "输入对方的地址与访问口令并开始连接。\n最多可同时查看 4 台远程电脑。"));
    }
    applyHostButton();
}

void EmptyState::setHostListening(bool listening)
{
    if (hostListening_ == listening)
        return;
    hostListening_ = listening;
    applyHostButton();
}

// 按钮的文案 / 图形 / 提示全由「本机是否在监听」决定。
// 单独成函数是因为它有两个调用方：状态变化（setHostListening）与语言切换
// （retranslate）。后者状态没变，走不了 setHostListening 的早退分支，但文案
// 照样得按当前语言重写一遍。
void EmptyState::applyHostButton()
{
    if (!hostButton_)
        return;

    const bool listening = hostListening_;

    // 监听中这颗按钮变成「禁止」：图形换圆圈斜杠、染 warning 色 ——
    // 机主隔一会儿回来，得一眼看出「现在不是邀请，是叫停」。
    hostButton_->setText(listening ? tr("禁止对方接入")
                                   : tr("允许对方接入"));
    hostButton_->setIcon(glyph::icon(listening ? glyph::Shape::Disconnect
                                               : glyph::Shape::MonitorIn,
                                     listening ? theme::colors().warning
                                               : theme::colors().textSecondary));
    hostButton_->setAccessibleName(hostButton_->text());
    hostButton_->setToolTip(listening
        ? tr("本机正在监听。点击停止监听，已连接的对方将全部断开。")
        : tr("本机进入监听状态，等待对方连接。\n点击后需确认监听端口与访问口令。"));
}

void EmptyState::updateIcon()
{
    if (!icon_)
        return;
    icon_->setPixmap(glyph::pixmap(glyph::Shape::Monitor, kIconSize,
                                   devicePixelRatioF(), theme::colors().textSecondary));
}

bool EmptyState::event(QEvent *event)
{
    // 跨缩放比拖动窗口（100% ↔ 150% 很常见）：构造时那张位图是按旧 DPR 栅格化的，
    // 不重画就会糊成一团。换主题时同理 —— 颜色换过之后这张位图还是旧色板。
    if (event->type() == QEvent::DevicePixelRatioChange)
        updateIcon();
    else if (event->type() == QEvent::LanguageChange)
        retranslate();
    return QWidget::event(event);
}

bool EmptyState::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        if (parentWidget())
            setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// 输入拦截（理由见头文件）
// ---------------------------------------------------------------------------
void EmptyState::mousePressEvent(QMouseEvent *event)        { event->accept(); }
void EmptyState::mouseReleaseEvent(QMouseEvent *event)      { event->accept(); }
void EmptyState::mouseDoubleClickEvent(QMouseEvent *event)  { event->accept(); }
void EmptyState::mouseMoveEvent(QMouseEvent *event)         { event->accept(); }
void EmptyState::wheelEvent(QWheelEvent *event)             { event->accept(); }
void EmptyState::keyPressEvent(QKeyEvent *event)            { event->accept(); }
void EmptyState::keyReleaseEvent(QKeyEvent *event)          { event->accept(); }
void EmptyState::contextMenuEvent(QContextMenuEvent *event) { event->accept(); }

} // namespace lanrdp
