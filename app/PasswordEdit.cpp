#include "app/PasswordEdit.h"

#include "app/Glyph.h"
#include "app/Theme.h"

#include <QResizeEvent>
#include <QToolButton>

namespace lanrdp {

namespace {

// 开关与输入框右边框的间距；也是「文字区域」额外让出的余量。
// 4px 是照着眼看定的：2px 时按钮几乎贴在边框上，看着像溢出到框外。
constexpr int kToggleInset = 4;

// 眼睛的边长。输入框本身约 26px 高，18 加上下内缩正好，再大就得压输入框高度。
constexpr int kToggleIconSize = 18;

} // namespace

PasswordEdit::PasswordEdit(QWidget *parent)
    : QLineEdit(parent)
{
    setEchoMode(QLineEdit::Password);

    toggle_ = new QToolButton(this);
    toggle_->setObjectName(QStringLiteral("passwordToggleButton"));
    toggle_->setCursor(Qt::PointingHandCursor);
    toggle_->setAutoRaise(true);
    toggle_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toggle_->setIconSize(QSize(kToggleIconSize, kToggleIconSize));
    // 不抢焦点：Tab 停在这颗眼睛上没有意义，而且按钮拿到焦点会先把回车吃掉，
    // 对话框的默认按钮就点不动了。
    toggle_->setFocusPolicy(Qt::NoFocus);

    connect(toggle_, &QToolButton::clicked, this,
            [this] { setPasswordVisible(!isPasswordVisible()); });

    syncToggle();
}

void PasswordEdit::setPasswordVisible(bool visible)
{
    setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    syncToggle();
}

void PasswordEdit::syncToggle()
{
    const bool visible = isPasswordVisible();

    // 图形表示「点一下会变成什么」：现在藏着，就画一只睁开的眼；已经是明文，
    // 就画一只划掉的眼。这样它跟以前「显示 / 隐藏」两个词说的是同一件事。
    toggle_->setIcon(glyph::icon(visible ? glyph::Shape::EyeOff : glyph::Shape::Eye,
                                 theme::colors().textSecondary));

    const QString tip = visible ? QStringLiteral("隐藏当前输入的内容")
                                : QStringLiteral("显示当前输入的内容");
    toggle_->setToolTip(tip);
    toggle_->setAccessibleName(tip);

    placeToggle();   // 图形换了，万一两种图形取整后宽度不同，按钮要重新摆
}

void PasswordEdit::resizeEvent(QResizeEvent *event)
{
    QLineEdit::resizeEvent(event);
    placeToggle();
}

void PasswordEdit::placeToggle()
{
    if (toggle_ == nullptr)
        return;

    const QSize hint = toggle_->sizeHint();
    // 别让按钮在极端窄宽下把整个输入框占满（正常情况下永远到不了这一档）。
    const int w = qMin(hint.width(), width() / 2);
    const int h = qMin(hint.height(), qMax(1, height() - kToggleInset * 2));
    toggle_->setGeometry(width() - w - kToggleInset, (height() - h) / 2, w, h);

    // 给文字让出按钮的位置 —— 否则长口令会滚到按钮底下，被盖住的那几位看不见。
    // ⚠️ 加变化判断：setTextMargins 会改 sizeHint，不判就会在本函数与 resize 之间来回。
    const int reserved = w + kToggleInset * 2;
    if (textMargins().right() != reserved)
        setTextMargins(0, 0, reserved, 0);
}

} // namespace lanrdp
