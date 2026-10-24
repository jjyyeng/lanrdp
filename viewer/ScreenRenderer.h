#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QWidget>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QKeyEvent;
class QMouseEvent;
class QMimeData;
class QWheelEvent;

namespace lanrdp {

// 远程画面显示控件：黑底、等比缩放、居中（letterbox）。
//
// 同时兼任控制端的输入采集面：开启 setInputEnabled 后把本控件上的
// 鼠标/键盘操作转换成平台无关的远程输入事件（发出信号，由上层封帧发送）。
class ScreenRenderer : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenRenderer(QWidget *parent = nullptr);

    void  setFrame(const QImage &frame);
    void  clear();

    QImage frame() const { return frame_; }
    QSize  frameSize() const { return frame_.size(); }
    bool   hasFrame() const { return !frame_.isNull(); }

    // 是否**允许**平滑缩放（双线性）。默认 true（画质优先）。
    // ⚠️ 真正是否开平滑还看缩放幅度 —— 死区里即使允许也会被 useSmoothScalingFor
    //    自动关掉（那时平滑几乎只烧 CPU，见该函数）。
    void setSmoothScaling(bool smooth);
    bool smoothScaling() const { return smooth_; }

    // 死区（面积比 > 1/9 的缩小）里默认关平滑以省 CPU。但一格独享 / 占满时画面
    // 只有这一份要画，省出来的 CPU 没人要，关掉反而让文字发虚 ⇒ 调用方（知道
    // 有几格可见的那一侧）据此允许死区也开平滑。多格时保持默认。
    void setSmoothInDeadZone(bool on);
    bool smoothInDeadZone() const { return smoothInDeadZone_; }

    // 综合「总开关 × 死区开关 × 缩放幅度」后的最终判定，paintEvent 用。
    // 抽成函数而不是内联在 paintEvent 里，是为了让测试直接钉住这条组合逻辑。
    bool smoothScalingFor(const QSize &frameSize, const QSize &targetSize) const;

    // 是否采集本控件的键鼠事件并转发为远程输入。
    // ⚠️ 关掉时会**自动补发全部抬起的键**（见 releaseAllPressed）—— 否则用户正按住的
    // Ctrl 会永久卡在远端。责任收在这里，谁关的开关都不必记得补。
    void setInputEnabled(bool enabled);
    bool inputEnabled() const { return inputEnabled_; }

    // 未启用键鼠、且已有画面时，画面中央会画一层「键鼠控制未启用 · 点此开启」。
    // 本查询返回「此刻该提示是否在显示」（没有画面 / 已启用时恒为 false）。
    // 抽成函数是因为提示是画出来的（paintEvent），测试没法直接查一个 QPainter 结果。
    bool inputHintVisible() const { return inputHintEnabled_ && !inputEnabled_ && !frame_.isNull(); }

    // 这一格**有没有远端可操作**（默认 true）。本机预览格必须置 false：它显示的是
    // 自己的桌面，画「点此操作对方电脑」是误导，点下去还会把本机键鼠交给一个
    // 没有连接的渲染器（事件进去了、没人接）。
    // ⚠️ 不能拿 inputHintVisible() 兼任这个判据 —— 预览格**有画面**，
    // 那条式子对它恒为 true。
    void setInputHintEnabled(bool enabled);
    bool inputHintEnabled() const { return inputHintEnabled_; }

    // 把当前**所有**按下的键 / 鼠标键各补一次抬起，然后清空集合。
    // 幂等（集合为空时是 no-op）。三个调用时机：关开关 / 关格 / 令牌丢失。
    // ⚠️ 刻意**不**在失焦时调用 —— Qt 里点悬浮条就会让本控件失焦，
    // 那会把用户正常按住的键松掉。
    void releaseAllPressed();

    // 无画面时的提示文字。
    // 控制端是「等待远程画面…」，被控端在预览关闭时是另一套文案 ——
    // 对被控端说「等待远程画面」语义不对，它自己就是画面源。
    void setIdleText(const QString &text);
    QString idleText() const { return idleText_; }

    // 这一格显示的是**最后一帧**（对端已断），不是活画面。
    // 画面**不清空**（用户还能看清远端断线前长什么样），改为压一层半透明暗色 +
    // 一句说明 —— 否则「停住的画面」看起来跟「卡住的活画面」一模一样。
    void setStale(bool on);
    bool isStale() const { return stale_; }
    void setStaleText(const QString &text);
    QString staleText() const { return staleText_; }

    // 纯函数：把 frameSize 等比缩放进 widgetSize 后的目标矩形（居中）。
    static QRect scaledTargetRect(const QSize &frameSize, const QSize &widgetSize);

    // 纯函数：这次缩放是否值得开平滑（双线性）。
    // 放大 / 大幅缩小（面积比 ≤ 1/9）⇒ true；死区（1/9 < 面积比 ≤ 1）⇒ false。
    // 依据：死区里缩放幅度有限，平滑对观感改善很小却占绘制开销的大部分。
    static bool useSmoothScalingFor(const QSize &frameSize, const QSize &targetSize);

    // 纯函数：控件坐标 -> 画面内归一化坐标（0..65535）。
    // 落在 letterbox 留白区或控件外时钳到边界；画面/控件尺寸非法时返回 false。
    static bool mapToNormalized(const QPoint &widgetPos,
                                const QSize  &frameSize,
                                const QSize  &widgetSize,
                                quint16      *nx,
                                quint16      *ny);

    // 纯函数：从拖拽的 MIME 数据中挑出第一个本地文件路径。
    // 不是文件拖拽（纯文本 / 仅远程 URL / 空）时返回空串。不做存在性检查。
    static QString firstLocalFile(const QMimeData *mime);

signals:
    void mouseMoved(quint16 nx, quint16 ny);
    // 按键 / 滚轮都带**最后一次**归一化坐标：被控端的 SendInput 对按键 / 滚轮
    // 默认打在系统当前光标处，不带坐标就会落在远端的旧位置。
    void mouseButtonChanged(int button, bool pressed, quint16 nx, quint16 ny);
    void mouseWheel(int delta, quint16 nx, quint16 ny);
    void keyChanged(int virtualKey, bool pressed, bool extended);

    // 用户在「键鼠控制未启用」的提示上点了一下 —— 上层据此打开总开关
    //（等价于去【设置…】勾选「启用远程键鼠控制」）。
    void inputEnableRequested();

    // 用户把本地文件拖到画面区并放下。path 是本地文件系统路径。
    void fileDropped(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    // 返回 true 表示已消费该事件（无论是否真的产生了远程事件）。
    bool handleKeyEvent(QKeyEvent *event, bool pressed);

    // 本控件坐标 -> 归一化坐标；拿不到（无画面 / 尺寸非法）时不做任何事。
    bool emitMove(const QPoint &widgetPos);

    QImage  frame_;
    bool    smooth_          = true;
    bool    smoothInDeadZone_ = false;
    bool    inputEnabled_    = false;
    bool    inputHintEnabled_ = true;   // 本格有远端可操作（预览格置 false）
    bool    dropActive_   = false;  // 拖拽悬停中：画一圈高亮描边
    QString idleText_     = QStringLiteral("等待远程画面…");  // 可换的空态文案

    // 当前按下的键 / 鼠标键（记账在 press，销账在 release）。
    // 存在的唯一理由：**打断时要能补发抬起** —— 没有这份集合就补不出来，
    // 远端那个键会永久按下（按住 Ctrl 时远端整机不可用）。
    QSet<int> pressedKeys_;     // Windows 虚拟键码
    QSet<int> pressedButtons_;  // Qt::MouseButton

    // 最后一次鼠标位置（归一化）。按键 / 滚轮载荷要带上它（注入端不认「当前光标」）。
    // 从没移过鼠标时为 (32768, 32768) = 正中，而不是 (0,0)（左上角）。
    quint16 lastNx_ = 32768;
    quint16 lastNy_ = 32768;

    // 最后一帧的灰显
    bool    stale_        = false;
    QString staleText_    = QStringLiteral(
        "● 已断开：画面停在最后一帧\n（条上可【重连】或【关闭】这一格）");
};

} // namespace lanrdp
