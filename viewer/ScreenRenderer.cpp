#include "viewer/ScreenRenderer.h"

#include "app/Theme.h"
#include "viewer/KeyMap.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QUrl>
#include <QWheelEvent>

namespace lanrdp {

namespace {

// 把像素坐标按 [origin, origin+size-1] 线性映射到 0..65535 并钳制。
quint16 normalizeAxis(int pixel, int origin, int size)
{
    if (size <= 1)
        return 0;

    qint64 scaled = (static_cast<qint64>(pixel) - origin) * 65535 / (size - 1);
    if (scaled < 0)
        scaled = 0;
    if (scaled > 65535)
        scaled = 65535;
    return static_cast<quint16>(scaled);
}

// 同一个物理键的另一套虚拟键码：小键盘 ↔ 主键盘。
//
// 为什么需要它：按下时戴着 `KeypadModifier`、映射成 VK_NUMPAD5；若按住期间
// NumLock 被切掉，抬起时的修饰符没了、映射成 '5'(0x35)。按 vk 记账就找不到
// 那笔按下 ⇒ 抬起被吞 ⇒ 远端那个键永久按下（用户视角：键盘坏了）。
// 抬起时用这个函数反查一次，把账销在**按下时**那个码上。
int alternateVk(int vk)
{
    // 小键盘 0-9 ↔ 主键盘 0-9
    if (vk >= 0x60 && vk <= 0x69)
        return 0x30 + (vk - 0x60);
    if (vk >= 0x30 && vk <= 0x39)
        return 0x60 + (vk - 0x30);

    // 运算符键：小键盘码 ↔ OEM 码（VK_MULTIPLY 没有主键盘对应，* 是 Shift+8）
    switch (vk) {
    case 0x6B: return 0xBB;   // VK_ADD      ↔ VK_OEM_PLUS
    case 0x6D: return 0xBD;   // VK_SUBTRACT ↔ VK_OEM_MINUS
    case 0x6E: return 0xBE;   // VK_DECIMAL  ↔ VK_OEM_PERIOD
    case 0x6F: return 0xBF;   // VK_DIVIDE   ↔ VK_OEM_2（/）
    case 0xBB: return 0x6B;
    case 0xBD: return 0x6D;
    case 0xBE: return 0x6E;
    case 0xBF: return 0x6F;
    default:   return 0;
    }
}

} // namespace

ScreenRenderer::ScreenRenderer(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 输入采集所需：跟踪移动（无需按键按住也能收到 mouseMove），
    // 且点击后能拿到键盘焦点。
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // 接受本地文件拖拽：拖到画面区即发送。
    setAcceptDrops(true);
}

void ScreenRenderer::setFrame(const QImage &frame)
{
    if (frame.isNull())
        return;
    frame_ = frame;
    update();
}

void ScreenRenderer::clear()
{
    frame_ = QImage();
    update();
}

void ScreenRenderer::setSmoothScaling(bool smooth)
{
    smooth_ = smooth;
    update();
}

void ScreenRenderer::setSmoothInDeadZone(bool on)
{
    if (smoothInDeadZone_ == on)
        return;
    smoothInDeadZone_ = on;
    update();
}

bool ScreenRenderer::smoothScalingFor(const QSize &frameSize, const QSize &targetSize) const
{
    if (!smooth_)
        return false;
    // 死区开关打开 = 不再用缩放幅度做减法（那一格值得用画质换 CPU）。
    if (smoothInDeadZone_)
        return true;
    return useSmoothScalingFor(frameSize, targetSize);
}

void ScreenRenderer::setInputEnabled(bool enabled)
{
    if (inputEnabled_ == enabled)
        return;
    inputEnabled_ = enabled;

    if (enabled) {
        setCursor(Qt::CrossCursor);
        setFocus(Qt::OtherFocusReason);
    } else {
        unsetCursor();
        // 关掉采集前先把按住的东西全松开 —— 否则远端那个键**永久按下**。
        // 放在这里（而不是调用方）是为了让「谁关的开关」都不必记得补。
        releaseAllPressed();
    }

    update();   // 提示层随开关出现 / 消失
}

// 本格有没有远端可操作。预览格置 false 后：不再画「点击画面即可操作对方电脑」，
// 点击也不再上报「请打开键鼠开关」。
void ScreenRenderer::setInputHintEnabled(bool enabled)
{
    if (inputHintEnabled_ == enabled)
        return;
    inputHintEnabled_ = enabled;
    update();   // 提示层可能因此出现 / 消失
}

// 补发全部抬起。顺序：先鼠标键、再键盘（与「先松手再松键」的直觉一致）。
// 幂等：集合为空时什么也不发。
void ScreenRenderer::releaseAllPressed()
{
    const QSet<int> buttons = pressedButtons_;
    pressedButtons_.clear();
    for (const int button : buttons)
        emit mouseButtonChanged(button, false, lastNx_, lastNy_);

    const QSet<int> keys = pressedKeys_;
    pressedKeys_.clear();
    for (const int vk : keys)
        emit keyChanged(vk, false, false);   // 补发抬起不区分扩展键：KEYEVENTF_KEYUP 已足够
}

// 无画面时的提示文案（控制端「等待远程画面…」/ 被控端「预览已关闭」）。
void ScreenRenderer::setIdleText(const QString &text)
{
    if (idleText_ == text)
        return;
    idleText_ = text;
    if (frame_.isNull())
        update();   // 只有当前确实没画面时才需要重绘
}

// 把「最后一帧」标灰。
// 刻意**不清画面** —— 断线后用户还想看清远端断线前是什么样子（停在登录框？
// 卡在某个对话框？），清成黑的等于把唯一线索也删了。
void ScreenRenderer::setStale(bool on)
{
    if (stale_ == on)
        return;
    stale_ = on;
    if (!frame_.isNull())
        update();   // 没画面时叠的层也画不出来，不必重绘
}

void ScreenRenderer::setStaleText(const QString &text)
{
    if (staleText_ == text)
        return;
    staleText_ = text;
    if (stale_ && !frame_.isNull())
        update();
}

QRect ScreenRenderer::scaledTargetRect(const QSize &frameSize, const QSize &widgetSize)
{
    if (!frameSize.isValid() || frameSize.isEmpty()
        || widgetSize.width() <= 0 || widgetSize.height() <= 0) {
        return QRect();
    }

    const qreal scale = qMin(static_cast<qreal>(widgetSize.width()) / frameSize.width(),
                             static_cast<qreal>(widgetSize.height()) / frameSize.height());

    const int width  = qMax(1, static_cast<int>(frameSize.width() * scale));
    const int height = qMax(1, static_cast<int>(frameSize.height() * scale));

    return QRect((widgetSize.width() - width) / 2, (widgetSize.height() - height) / 2, width, height);
}

bool ScreenRenderer::useSmoothScalingFor(const QSize &frameSize, const QSize &targetSize)
{
    const qint64 frameArea  = qint64(frameSize.width())  * frameSize.height();
    const qint64 targetArea = qint64(targetSize.width()) * targetSize.height();
    if (frameArea <= 0 || targetArea <= 0)
        return true;  // 尺寸非法：兜底按画质优先（正常路径已被 target.isValid() 守卫）

    // 放大：必须开平滑，否则马赛克块明显。
    if (targetArea > frameArea)
        return true;

    // 缩小：面积比 ≤ 1/9（幅度 > 3x）时双线性显著抗锯齿 ⇒ 开。
    // 面积比落在 (1/9, 1]（与显示预算同一守卫线）时缩放幅度有限，平滑对观感改善很小
    // 却占绘制开销的大部分（实测 4K→1x1 单格：开 7.91 ms/帧 vs 关 2.98 ms）⇒ 关。
    return targetArea * 9 <= frameArea;
}

bool ScreenRenderer::mapToNormalized(const QPoint &widgetPos,
                                     const QSize  &frameSize,
                                     const QSize  &widgetSize,
                                     quint16      *nx,
                                     quint16      *ny)
{
    if (!nx || !ny)
        return false;

    const QRect target = scaledTargetRect(frameSize, widgetSize);
    if (!target.isValid() || target.width() <= 0 || target.height() <= 0)
        return false;

    *nx = normalizeAxis(widgetPos.x(), target.left(), target.width());
    *ny = normalizeAxis(widgetPos.y(), target.top(), target.height());
    return true;
}

QString ScreenRenderer::firstLocalFile(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls())
        return QString();

    const QList<QUrl> urls = mime->urls();
    for (const QUrl &url : urls) {
        // 跳过 http(s) 之类的远程 URL：它们不是可发送的本地文件。
        if (!url.isLocalFile())
            continue;

        const QString path = url.toLocalFile();
        if (!path.isEmpty())
            return path;
    }
    return QString();
}

void ScreenRenderer::dragEnterEvent(QDragEnterEvent *event)
{
    // 只有「拖进了本地文件」才接受；纯文本 / URL 拖拽不接管，
    // 否则会把普通拖放也吃进来。
    if (firstLocalFile(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();
    dropActive_ = true;
    update();
}

void ScreenRenderer::dragLeaveEvent(QDragLeaveEvent *event)
{
    event->accept();
    if (dropActive_) {
        dropActive_ = false;
        update();
    }
}

void ScreenRenderer::dropEvent(QDropEvent *event)
{
    const QString path = firstLocalFile(event->mimeData());

    if (dropActive_) {
        dropActive_ = false;
        update();
    }

    if (path.isEmpty()) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();
    emit fileDropped(path);
}

void ScreenRenderer::paintEvent(QPaintEvent *)
{
    const lanrdp::theme::Colors &c = lanrdp::theme::colors();

    QPainter painter(this);
    painter.fillRect(rect(), c.canvas);

    if (frame_.isNull()) {
        // 空态文字用「说明文字」那一档：它比正文弱，但仍要能一眼读出来 ——
        // 这是用户打开软件看到的第一句话。
        painter.setPen(c.textSecondary);
        painter.drawText(rect(), Qt::AlignCenter, idleText_);
    } else {
        const QRect target = scaledTargetRect(frame_.size(), size());
        if (target.isValid()) {
            // 死区里默认关平滑（实测省 ~2.8x 绘制耗时）；一格独享时由
            // smoothInDeadZone_ 豁免（见 smoothScalingFor）。
            painter.setRenderHint(QPainter::SmoothPixmapTransform,
                                  smoothScalingFor(frame_.size(), target.size()));
            painter.drawImage(target, frame_);

            if (stale_) {
                // 最后一帧 —— 压一层暗色 + 明说状态，别让人以为只是画面卡住了。
                painter.fillRect(target, QColor(0, 0, 0, 150));
                painter.setPen(QColor(235, 205, 130));
                painter.drawText(target, Qt::AlignCenter, staleText_);
            } else if (inputEnabled_) {
                // 明确提示「此刻你的键鼠正在操作远端」，避免误操作。
                // ⚠️ 文案必须与实现一致：`viewer/KeyMap.cpp` 只覆盖**常用键** ——
                // 媒体键 / F13–F24 / IME 组字等映射不出来（`qtKeyToVirtualKey` 返回 0），
                // 那些键会被**本地丢弃**（不是发给被控端）。所以不能写「所有按键」。
                // 但确实没有「本地快捷键截获」：连 Escape 也照发（唯一的例外是
                // 占满态下的 Esc，由 RemoteView 本地消费）。
                // 想整体停在本机请用【设置…】里的键鼠总开关。
                painter.setPen(QColor(120, 220, 120));
                painter.drawText(target.adjusted(6, 4, -6, 0),
                                 Qt::AlignTop | Qt::AlignLeft,
                                 QStringLiteral("● 键鼠已接管，操作会发送到对方"));
            } else if (inputHintVisible()) {
                // 反向提示：**有画面但键鼠没开**。这时输入会被静默丢弃，
                // 用户只看得到「已获得输入权」却点什么都没反应 —— 必须说出来，
                // 并且给一个「点这里就能开」的出口。
                // ⚠️ 只在有画面时画：空态已有 idleText_，再叠一层会打架。
                painter.fillRect(target, QColor(0, 0, 0, 90));
                painter.setPen(QColor(240, 220, 150));
                painter.drawText(target, Qt::AlignCenter,
                                 QStringLiteral("点击画面，即可控制对方电脑"));
            }
        }
    }

    if (dropActive_) {
        // 拖拽悬停中：描一圈高亮，告诉用户「可以放这里」
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(120, 200, 255), 3, Qt::DashLine));
        painter.drawRect(rect().adjusted(2, 2, -3, -3));
    }
}

// 画面坐标 -> 归一化坐标并记下「最后一次位置」。没画面 / 尺寸非法时返回 false。
bool ScreenRenderer::emitMove(const QPoint &widgetPos)
{
    quint16 nx = 0;
    quint16 ny = 0;
    if (!mapToNormalized(widgetPos, frame_.size(), size(), &nx, &ny))
        return false;
    lastNx_ = nx;
    lastNy_ = ny;
    emit mouseMoved(nx, ny);
    return true;
}

void ScreenRenderer::mouseMoveEvent(QMouseEvent *event)
{
    if (!inputEnabled_) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    emitMove(event->position().toPoint());
    event->accept();
}

void ScreenRenderer::mousePressEvent(QMouseEvent *event)
{
    if (!inputEnabled_) {
        // 未启用时，画面上那层「点此开启」提示是可以点的 —— 这就是那个出口。
        // 仍然要记下位置：开启后第一次点击的落点就是这里（否则会打到远端旧光标处）。
        if (inputHintVisible()) {
            emitMove(event->position().toPoint());
            setFocus(Qt::MouseFocusReason);   // 让它能接住随后的键盘
            event->accept();
            emit inputEnableRequested();
            return;
        }

        // 本格没有远端可操作（本机预览格）：那层提示根本没画，点击也没有下游。
        // 就地消化 —— 否则会顺着「点提示 = 开总开关」那条路把整个窗口的键鼠
        // 交给一个没有连接的渲染器，本机点击原地消失。
        event->accept();
        return;
    }

    setFocus(Qt::MouseFocusReason);  // 点一下画面即取得键盘焦点
    emitMove(event->position().toPoint());   // 先落点，再按键（顺序反了会打在旧位置）
    pressedButtons_.insert(static_cast<int>(event->button()));
    emit mouseButtonChanged(static_cast<int>(event->button()), true, lastNx_, lastNy_);
    event->accept();
}

void ScreenRenderer::mouseReleaseEvent(QMouseEvent *event)
{
    // ⚠️ 抬起**不受开关门控**：关掉开关后用户松手，这个抬起仍然要发出去，
    // 否则远端那个键会永久按下（这正是「远端卡键」这条缺陷的另一半）。
    if (!inputEnabled_ && !pressedButtons_.contains(static_cast<int>(event->button()))) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    pressedButtons_.remove(static_cast<int>(event->button()));
    emit mouseButtonChanged(static_cast<int>(event->button()), false, lastNx_, lastNy_);
    event->accept();
}

void ScreenRenderer::wheelEvent(QWheelEvent *event)
{
    if (!inputEnabled_) {
        QWidget::wheelEvent(event);
        return;
    }

    emitMove(event->position().toPoint());   // 滚轮也要落在当前光标处
    const int delta = event->angleDelta().y();
    if (delta != 0)
        emit mouseWheel(delta, lastNx_, lastNy_);

    event->accept();
}

bool ScreenRenderer::handleKeyEvent(QKeyEvent *event, bool pressed)
{
    // ⚠️ 抬起（pressed == false）**不受开关门控**：关掉开关后用户松手，
    // 这个抬起仍要发出去，否则远端那个键永久按下。
    // 判据用「这个键是不是我们记着按下的」—— 没记过的抬起没必要发（也免得凭空造事件）。
    const int qtKey = event->key();
    const std::uint16_t vk = qtKeyToVirtualKey(qtKey, event->modifiers());

    if (!pressed) {
        if (vk != 0 && pressedKeys_.contains(static_cast<int>(vk))) {
            pressedKeys_.remove(static_cast<int>(vk));
            emit keyChanged(static_cast<int>(vk), false, qtKeyIsExtended(qtKey, event->modifiers()));
            return true;
        }

        // 按住期间 NumLock 被切：抬起映射成了另一个码（小键盘 ↔ 主键盘），
        // 按 vk 找不到那笔按下。这里反查一次，把账销在**按下时**那个 vk 上 ——
        // 否则抬起被吞，远端那个键永久按下。
        const int alt = alternateVk(static_cast<int>(vk));
        if (alt != 0 && pressedKeys_.contains(alt)) {
            pressedKeys_.remove(alt);
            emit keyChanged(alt, false, qtKeyIsExtended(qtKey, event->modifiers()));
            return true;
        }

        return inputEnabled_;   // 没记过的抬起：开关关着就当没看见
    }

    if (!inputEnabled_)
        return false;

    // 不支持的键交给上层（例如留着以后做本地快捷键）。
    // ⚠️ 这条必须排在 autoRepeat 之前：否则首次按下冒泡、长按重复却被吞，
    // 同一颗键前后不一致（上层看到「按下」，却拿不到重复）。
    if (vk == 0)
        return false;

    // 长按产生的重复按键由被控端系统自行产生，这里只转发首次按下，避免重复注入
    if (event->isAutoRepeat())
        return true;

    pressedKeys_.insert(static_cast<int>(vk));
    emit keyChanged(static_cast<int>(vk), true, qtKeyIsExtended(qtKey, event->modifiers()));
    return true;
}

void ScreenRenderer::keyPressEvent(QKeyEvent *event)
{
    if (handleKeyEvent(event, true))
        return;
    QWidget::keyPressEvent(event);
}

void ScreenRenderer::keyReleaseEvent(QKeyEvent *event)
{
    if (handleKeyEvent(event, false))
        return;
    QWidget::keyReleaseEvent(event);
}

} // namespace lanrdp
