#pragma once

#include <QWidget>

#include <memory>

class QToolButton;

namespace lanrdp {

class ClientConnection;
class EmptyState;
class ScreenOverlayBar;
class ScreenRenderer;

// 一控多 —— **一格**的封装。
//
// 一格 = 画面（`ScreenRenderer`）+ 顶部悬浮条（`ScreenOverlayBar`）+ 条上的控件，
// 外加可选的**一条**控制端连接（`ClientConnection`）。
// · 有连接：每格一条连接，几台远端就是几格（1×1 / 1×2 / 左大右两小 / 2×2）；
// · 没连接：空态（`EmptyState` 盖在画面上，给「连接对方 / 让对方连进来」两个入口）。
//
// 被控端的状态指示（「已连 N 个」）在每格悬浮条**最左**：被控是全局功能，
// 状态跟条走不跟状态栏走（状态栏放它会导致频繁显隐闪烁）。监听开关与
// 收回操作权仍在菜单栏 —— 那是动作，不是状态。
//
// 为什么单独成类：一控多要求「一格里的一整套东西」能有 N 份。若照旧做成 `MainWindow`
// 的单份成员，一控多就得换成十几个**平行数组**（N 个 renderer / N 个按钮…），
// 「谁跟谁是同一格」会散落在十几处下标里 —— 那正是把连接状态收进
// `ClientConnection` 时解决过的问题，同一个道理换个层次再解决一次。
//
// ⚠️ 本类**不负责接线**：信号连到哪、控件按什么规则显隐，仍然是 `MainWindow` 的事
// ——「这一格现在是什么状态」只有它知道。这里只提供零件、状态位与
// 「用户点了这一格」这一个信号。
//
// ⚠️ 条上控件的 objectName 刻意沿用旧名
// （`disconnectButton` / `localPreviewCheck`…）。面板、条换了承载，
// 但「这个按钮叫什么」与承载无关；沿用旧名让那批断言不必改写。
class RemoteView : public QWidget
{
    Q_OBJECT

public:
    explicit RemoteView(QWidget *parent = nullptr);
    ~RemoteView() override;

    // ---- 零件 ----
    ScreenRenderer   *renderer() const { return renderer_; }
    ScreenOverlayBar *bar() const { return bar_; }
    // 空态层（没有连接对象时盖在画面上，给「连接对方 / 让对方连进来」两个入口）。
    // 显隐由 MainWindow 按这格的连接状态刷 —— 它才知道该不该显示。
    EmptyState       *emptyState() const { return emptyState_; }

    // 条上控件的全集（按显示顺序建好）。哪几个可见由 MainWindow 按这格的
    // 连接状态定 —— 每格一批按钮，各管各的连接。
    // 「已连 N 个」是唯一的例外：它是**全局**被控状态，显隐由 MainWindow 按监听状态刷。
    QToolButton *clientListButton() const { return clientListButton_; }
    QToolButton *connectButton() const { return connectButton_; }
    QToolButton *sendFileButton() const { return sendFileButton_; }
    QToolButton *disconnectButton() const { return disconnectButton_; }
    QToolButton *releaseTokenButton() const { return releaseTokenButton_; }

    // 操作权状态的指示灯（控制端那颗指针图标，**不可点**，只有图形与悬停提示）。
    QToolButton *tokenStatusButton() const { return tokenStatusButton_; }

    // 控制端的操作权状态。三态各自的图形 / 提示语都在这里定，
    // 上层只报「现在是什么状态」，不碰图标细节。
    enum class TokenState {
        NotConnected,   // 还没连上（或者刚连上、还在握手）
        Waiting,        // 连上了、但操作权不在自己手里
        Held,           // 手上握着操作权，可以操作对方
    };
    void setTokenState(TokenState state);
    TokenState tokenState() const { return tokenState_; }

    // 【占满 / 还原】（条上最左）。与【断开】一样是**按格**动作 ——
    // 点第 2 格的它就把第 2 格铺满，与「当前选中哪一格」无关。
    QToolButton *maximizeButton() const { return maximizeButton_; }

    // 断线格的两个出口（**只在断线时出现**）。
    // · 【重连】= 用记下的参数立刻再试一次（不等退避计时）；
    // · 【关闭】= 扔掉这一格（只剩一格时退化为「复位成空态」）。
    QToolButton *retryButton() const { return retryButton_; }
    QToolButton *closeCellButton() const { return closeCellButton_; }

    // ---- 这一格的那条连接（可为空 = 空态格）----
    // 所有权在本类手里（unique_ptr）：一格的东西跟一格一起生灭，
    // 不会出现「格子没了、连接还在跑」这种要靠人记的约束。
    ClientConnection *connection() const { return connection_.get(); }
    bool hasConnection() const { return connection_ != nullptr; }

    // 接管一条连接。传 nullptr = 摘掉（旧的会被销毁）。
    // 调用方负责在摘掉前做内核清理（teardown），本类不做网络动作。
    void setConnection(std::unique_ptr<ClientConnection> conn);

    // 交还所有权（关掉这一格时用）。之后 connection() 为 nullptr。
    std::unique_ptr<ClientConnection> takeConnection();

    // ---- 选中态（「点哪一格就操作哪一格」）----
    // 只是显示（条上的强调线）；真正的路由由 MainWindow 按「选中索引」决定。
    void setSelected(bool on);
    bool isSelected() const { return selected_; }

    // ---- 占满态 ----
    // 这里只存一份**镜像**用于显示（按钮文案）与 Esc 的判定，**权威状态在 `ViewGrid`**
    // ——它是真正决定谁铺满、谁 hide() 的那个人。`setMaximized()` 由 MainWindow 在
    // 切换占满之后逐格同步（同 setSelected 的做法）。
    void setMaximized(bool on);
    bool isMaximized() const { return maximized_; }

    // ---- 断线态 ----
    // 断线 = 「这条连接记着某个对端、但此刻没连着」。表现是**保留最后一帧 + 灰显**
    // 加条上两个出口（【重连】/【关闭】）。画面**不清** —— 那正是这个状态的意义。
    // `staleText` 是叠在最后一帧上的说明（「已断开」与「正在重连…」措辞不同，
    // 由上层按连接的退避状态给）。
    void setDisconnected(bool on, const QString &staleText = QString());
    bool isDisconnected() const { return disconnected_; }

    // 条标题：有连接写对端地址，空格写「未连接 / 第 N 格」。
    void    setTitle(const QString &text);
    QString title() const;

signals:
    // 用户在这一格里按了鼠标（画面区或条上都算）。上层据此把它设为选中格。
    // 注意：**不消费事件** —— 画面区的按键仍要照常转发远端（见 .cpp 的 eventFilter）。
    void activated(RemoteView *self);

    // 占满态下用户按了 Esc，请求还原。上层把它转成
    // `ViewGrid::setMaximizedIndex(-1)`。
    // 与 activated 不同，这一条**会消费掉那颗 Esc** —— 它是纯粹的本地手势，
    // 发了信号就不该再漏给远端（远端 Vim / 对话框里的 Esc 是它自己的事，
    // 那由「没占满」这条前提保证）。
    void restoreRequested(RemoteView *self);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();

    // 按 maximized_ 刷那颗图标按钮（图形 + 悬停提示 + 无障碍名字）。
    // 创建时和状态切换时都走它 —— 两处的说明文字才不会各写一份、各自走样。
    void refreshMaximizeButton();

    // 同上三条，对应 tokenState_。
    void refreshTokenStatus();

    ScreenRenderer   *renderer_   = nullptr;
    ScreenOverlayBar *bar_        = nullptr;
    EmptyState       *emptyState_ = nullptr;

    QToolButton *clientListButton_   = nullptr;  // 「已连 N 个」（条上最左，监听中才显示）
    QToolButton *maximizeButton_     = nullptr;  // 占满 / 还原
    QToolButton *connectButton_      = nullptr;  // 连接另一台（只加一格空视窗）
    QToolButton *sendFileButton_     = nullptr;  // 发送文件…
    QToolButton *disconnectButton_   = nullptr;  // 断开
    QToolButton *retryButton_        = nullptr;  // 重连
    QToolButton *closeCellButton_    = nullptr;  // 关闭
    QToolButton *releaseTokenButton_ = nullptr;  // 让出操作权
    QToolButton *tokenStatusButton_  = nullptr;  // 操作权状态（不可点）

    std::unique_ptr<ClientConnection> connection_;
    bool selected_     = false;
    bool maximized_    = false;
    bool disconnected_ = false;   // 断线格（保留最后一帧 + 灰显）
    TokenState tokenState_  = TokenState::NotConnected;

    // 占满态下那颗 Esc 已被本地吃掉 —— 它对应的 keyRelease 也得一起吃掉，
    // 否则远端会收到一颗「没有按下、只有抬起」的 Esc。不能拿 maximized_ 代替：
    // 还原之后 maximized_ 立刻变 false，那颗 release 就漏出去了。
    bool escConsumed_ = false;
};

} // namespace lanrdp
