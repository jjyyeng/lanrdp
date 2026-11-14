#include "viewer/ModifierKeyGuard.h"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QTimer>

#include "viewer/ScreenRenderer.h"

namespace lanrdp {

ModifierKeyGuard::ModifierKeyGuard(QObject *parent)
    : QObject(parent)
{
}

bool ModifierKeyGuard::isAltKey(const QKeyEvent *event)
{
    // Key_AltGr：部分布局把右 Alt 报成它，对远端来说同样是 Alt，不能漏。
    return event->key() == Qt::Key_Alt || event->key() == Qt::Key_AltGr;
}

void ModifierKeyGuard::relay(ScreenRenderer *renderer, QKeyEvent *event)
{
    relaying_ = true;
    QKeyEvent copy(event->type(), event->key(), event->modifiers(), event->nativeScanCode(),
                   event->nativeVirtualKey(), event->nativeModifiers(), event->text(),
                   event->isAutoRepeat(), event->count());
    QCoreApplication::sendEvent(renderer, &copy);
    relaying_ = false;
}

bool ModifierKeyGuard::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress: {
        auto *key = static_cast<QKeyEvent *>(event);
        if (relaying_ || !isAltKey(key))
            return false;

        // 只有「画面控件持有焦点」时才接管：否则是用户在对话框里按 Alt，
        // 那是本机界面自己的事。
        auto *renderer = qobject_cast<ScreenRenderer *>(QApplication::focusWidget());
        if (!renderer)
            return false;

        target_ = renderer;
        relay(renderer, key);
        return true;   // 吞掉：菜单栏拿不到就不会去抢焦点
    }

    case QEvent::KeyRelease: {
        auto *key = static_cast<QKeyEvent *>(event);
        if (relaying_ || !isAltKey(key) || !target_)
            return false;

        ScreenRenderer *renderer = target_.data();
        target_.clear();
        relay(renderer, key);
        return true;
    }

    case QEvent::FocusOut: {
        if (relaying_)
            return false;
        auto *renderer = qobject_cast<ScreenRenderer *>(watched);
        if (!renderer)
            return false;
        if (static_cast<QFocusEvent *>(event)->reason() != Qt::MenuBarFocusReason)
            return false;

        // 菜单栏还是把焦点抢走了：不抢回来，键盘就不再发往远端。
        // 用 singleShot 排队，避免在焦点切换过程中重入。
        QTimer::singleShot(0, renderer, [renderer]() {
            renderer->setFocus(Qt::OtherFocusReason);
        });
        return false;   // 焦点事件本身照常送达
    }

    default:
        return false;
    }
}

} // namespace lanrdp
