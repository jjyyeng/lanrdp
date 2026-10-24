#include "viewer/EmptyState.h"

#include "app/Glyph.h"
#include "app/Theme.h"

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
    icon_->setPixmap(glyph::pixmap(glyph::Shape::Monitor, kIconSize,
                                   devicePixelRatioF(), theme::colors().textSecondary));
    icon_->setAlignment(Qt::AlignCenter);
    root->addWidget(icon_, 0, Qt::AlignHCenter);

    root->addSpacing(14);

    title_ = new QLabel(QStringLiteral("尚未连接"), this);
    title_->setObjectName(QStringLiteral("emptyTitle"));
    title_->setAlignment(Qt::AlignCenter);
    root->addWidget(title_);

    root->addSpacing(6);

    subtitle_ = new QLabel(QStringLiteral("连接远程电脑，或允许远程电脑连接本机"), this);
    subtitle_->setObjectName(QStringLiteral("emptySubtitle"));
    subtitle_->setAlignment(Qt::AlignCenter);
    root->addWidget(subtitle_);

    root->addSpacing(20);

    auto *buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(10);

    connectButton_ = new QPushButton(QStringLiteral("连接远程电脑"), this);
    connectButton_->setObjectName(QStringLiteral("emptyPrimary"));
    connectButton_->setCursor(Qt::PointingHandCursor);
    // 图标颜色跟文字走：主按钮的文字是 onAccent，次按钮是 textSecondary。
    // 两个图标都指向屏幕 —— 一个箭头朝外（我去连别人），一个朝内（别人连进来）。
    connectButton_->setIcon(glyph::icon(glyph::Shape::MonitorOut, theme::colors().onAccent));
    connectButton_->setIconSize(QSize(kButtonIcon, kButtonIcon));
    connectButton_->setToolTip(QStringLiteral(
        "输入对方的地址与访问口令并开始连接。\n最多可同时查看 4 台远程电脑。"));
    connect(connectButton_, &QPushButton::clicked, this, &EmptyState::connectRequested);

    hostButton_ = new QPushButton(QStringLiteral("允许对方接入"), this);
    hostButton_->setObjectName(QStringLiteral("emptySecondary"));
    hostButton_->setCursor(Qt::PointingHandCursor);
    hostButton_->setIcon(glyph::icon(glyph::Shape::MonitorIn, theme::colors().textSecondary));
    hostButton_->setIconSize(QSize(kButtonIcon, kButtonIcon));
    hostButton_->setToolTip(QStringLiteral(
        "本机进入监听状态，等待对方连接。\n点击后需确认监听端口与访问口令。"));
    connect(hostButton_, &QPushButton::clicked, this, &EmptyState::hostRequested);

    buttons->addStretch(1);
    buttons->addWidget(connectButton_);
    buttons->addWidget(hostButton_);
    buttons->addStretch(1);
    root->addLayout(buttons);

    root->addStretch(1);

    setStyleSheet(theme::emptyStateStyleSheet());

    if (parent) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
    }

    hide();   // 显隐由 MainWindow 按模式统一刷
}

void EmptyState::setHostListening(bool listening)
{
    if (hostListening_ == listening)
        return;
    hostListening_ = listening;

    // 监听中这颗按钮变成「禁止」：图形换圆圈斜杠、染 warning 色 ——
    // 机主隔一会儿回来，得一眼看出「现在不是邀请，是叫停」。
    hostButton_->setText(listening ? QStringLiteral("禁止对方接入")
                                   : QStringLiteral("允许对方接入"));
    hostButton_->setIcon(glyph::icon(listening ? glyph::Shape::Disconnect
                                               : glyph::Shape::MonitorIn,
                                     listening ? theme::colors().warning
                                               : theme::colors().textSecondary));
    hostButton_->setAccessibleName(hostButton_->text());
    hostButton_->setToolTip(listening
        ? QStringLiteral("本机正在监听。点击停止监听，已连接的对方将全部断开。")
        : QStringLiteral("本机进入监听状态，等待对方连接。\n点击后需确认监听端口与访问口令。"));
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
