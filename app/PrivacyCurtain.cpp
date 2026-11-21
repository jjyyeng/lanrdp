#include "app/PrivacyCurtain.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRect>
#include <QScreen>
#include <QShowEvent>

namespace lanrdp {

PrivacyCurtain::PrivacyCurtain(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
{
    setObjectName(QStringLiteral("privacyCurtain"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);   // 自己整块刷黑，不必先擦背景
    setFocusPolicy(Qt::StrongFocus);               // Esc 要在本窗收到
    setCursor(Qt::ArrowCursor);
    setWindowTitle(tr("已遮挡"));
}

void PrivacyCurtain::showOverVirtualDesktop()
{
    // 多屏取并集：主屏之外的每一块也必须被盖住。
    QRect area;
    const QScreen *primary = QGuiApplication::primaryScreen();
    for (const QScreen *screen : QGuiApplication::screens())
        area = area.united(screen->geometry());
    if (area.isEmpty() && primary)
        area = primary->geometry();
    if (area.isEmpty())
        area = QRect(0, 0, 1024, 768);   // 极端兜底：没有屏信息也不至于画出一块 0 尺寸的窗

    setGeometry(area);
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void PrivacyCurtain::setHotkeyText(const QString &text)
{
    if (hotkeyText_ == text)
        return;
    hotkeyText_ = text;
    update();
}

void PrivacyCurtain::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    // 纯黑是功能本体，**不走主题**：浅色主题下也必须是全黑。
    painter.fillRect(rect(), Qt::black);

    const int unit = qMax(8, height() / 70);   // 跟随屏幕高度缩放，4K 下不至于变成一行小字

    QFont titleFont = font();
    titleFont.setPointSizeF(unit * 2.2);
    titleFont.setBold(true);

    QFont hintFont = font();
    hintFont.setPointSizeF(unit * 0.9);

    const QRect  titleRect(0, height() / 2 - unit * 5, width(), unit * 4);
    const QRect  hintRect(0, height() / 2 + unit * 1, width(), unit * 3);

    QColor dim(255, 255, 255, 120);

    painter.setFont(titleFont);
    painter.setPen(Qt::white);
    painter.drawText(titleRect, Qt::AlignHCenter | Qt::AlignVCenter, tr("已遮挡"));

    painter.setFont(hintFont);
    painter.setPen(dim);

    QString hint = hotkeyText_.isEmpty() ? tr("点一下恢复") : tr("按 %1 或点一下恢复").arg(hotkeyText_);
    painter.drawText(hintRect, Qt::AlignHCenter | Qt::AlignVCenter, hint);

    // 遮挡期间的第二个事实：对方既看不到新画面，也操作不了。
    // 写在提示下面一行，免得机主以为「只是自己看不见」。
    painter.drawText(hintRect.translated(0, unit * 2),
                     Qt::AlignHCenter | Qt::AlignVCenter,
                     tr("对方画面已定格，操作权已收回"));
}

void PrivacyCurtain::mousePressEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    emit dismissed();
}

void PrivacyCurtain::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        emit dismissed();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PrivacyCurtain::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 挡住全屏后仍要能被键盘找到（否则 Esc 这条逃生路形同不存在）。
    setFocus(Qt::OtherFocusReason);
}

} // namespace lanrdp
