#pragma once

#include "app/Dialogs.h"
#include "app/LogPort.h"
#include "app/SessionStore.h"
// 值成员要用到 `HostController::UiState`（宿主状态的本地快照），所以这里必须见到完整定义。
#include "host/HostController.h"
#include "protocol/InputEvent.h"

#include <QMainWindow>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <unordered_map>

class QAction;
class QLabel;
class QMenu;
class QProgressBar;
class QResizeEvent;  // resizeEvent 覆写用
class QShowEvent;    // 首次显示时补一次显示预算下发
class QThread;       // 宿主的工作线程
class QTimer;
class QToolButton;   // onClientListMenu 的锚点按钮

namespace lanrdp {
class AuthClient;
class ClientConnection;
class ClipboardSync;
class FileReceiver;
class FileSender;
class H264Decoder;
class RemoteView;
class ScreenOverlayBar;
class ScreenStreamer;
class ScreenRenderer;
class Session;
class ViewGrid;
}

// 双角色单窗口：既能当被控端（监听并推送画面），也能当控制端（连接并显示画面）。
// 两个角色**可以同时在线**：被控是全局功能（菜单【被控】+ 状态栏指示），
// 控制端是格子化的画面（1..4 格），互不抢占窗口。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // `logFilePath` 为空 = 用默认（exe 同目录的 lanrdp.log）。给测试 / CLI 留的注入缝：
    // 否则所有实例与 30+ 个 UI 用例都往同一个 <build>/lanrdp.log 追加并触发 5 MB 轮转。
    //
    // `sessionFilePath` 为空 = **不持久化**会话参数，`main()` 才传真实路径。
    // 默认关掉是有意的：30+ 个 UI 用例若默认读写真实配置，会互相串味，还会把开发机
    // 上正在用的口令冲掉 —— 那种失败极难归因。程序自己那份由 `main()` 显式打开。
    explicit MainWindow(const QString &logFilePath = QString(),
                        const QString &sessionFilePath = QString(),
                        QWidget *parent = nullptr);
    ~MainWindow() override;

    // 程序化入口（供命令行自测 / 自动化复用）。
    void startHost(quint16 port, const QString &password);
    void startConnect(const QString &host, quint16 port, const QString &password);

    // 停止监听（断开全部控制端但保留窗口，可再次开始监听）。
    void stopHost();
    bool isHosting() const;

    // 本机键鼠是否已被机主锁定（读工作线程推来的快照，GUI 侧随时可查）。
    bool inputLocked() const { return hostState_.inputLocked; }

    // 程序化发送一次鼠标移动（归一化 0..65535），命令行自测用。
    void sendMouseMove(quint16 nx, quint16 ny);

    // 程序化发送一次鼠标按键（button: 0=左），命令行自测 / 单测用。
    // 和鼠标移动不同，按键是「真的想操作」——没有输入权时它会去申请输入令牌，
    // 是「收回输入权后点一下就能拿回来」这条路径的入口。
    void sendMouseButton(int button, bool pressed);

    // 被控端画面编码方式（命令行自测用；UI 上在【开始监听…】对话框里）。
    void setHostCodec(bool useH264, int bitrateBps = 8 * 1000 * 1000);
    bool hostUsesH264() const { return hostUseH264_; }

    // 被控端接收文件的落盘目录（不带则用 defaultReceiveDir()：系统「下载」目录）。
    void setReceiveDir(const QString &dir);
    QString receiveDir() const;

    // 控制端把本地文件发给对端。返回 false 表示还没连上或文件打不开。
    bool sendFile(const QString &path);

    // 控制端收尾：等发送缓冲排空后**半关**写方向（只发 FIN，不关读方向）。
    // 关闭完成（对端也关掉它那一侧）时 emit clientClosed()。
    // 给「一次性动作做完就退」的命令行用（现用于 --send-file）。两件别想当然的事：
    // · `fileSent` 只表示「帧已排进 Qt 的用户态写缓冲」—— 几 MB 的文件在这一刻
    //   缓冲里还压着大半个文件，所以「等一个固定毫秒数再退出」必然不可靠；
    // · 缓冲排空后直接 `disconnectFromHost()` 也不行：Windows 关闭一个**读缓冲非空**
    //   的 socket 会回 RST（对端一直在推画面，读缓冲几乎总是非空），RST 会连带丢掉
    //   本方还没被确认的发送队列 —— 对端只收到文件头就报「传输中断」。
    //   半关发的是 FIN，不动发送队列，也不触发 RST。
    // 调用一次即可（重复调用只是多接一次信号，行为不变）。
    // ⚠️ 只适合「半关之后就不再需要这条连接」的场景。半关会让 socket 走到
    //   disconnected，而 ClientConnection 的自动重连默认是开着的 —— 断线对它来说
    //   与网络故障没有区别，会照常排下一次重连。现在唯一调用方是命令行发完文件即退，
    //   进程根本活不到重连那一刻；将来要在界面里复用它，必须先把自动重连关掉。
    void halfCloseClientWhenFlushed();

    // 这三个查询都转发给 connection_。定义放 .cpp ——
    // ClientConnection 在头文件里只有前向声明，内联会报 incomplete type。
    bool clientReady() const;
    // 控制端是否「允许再自动申请一次输入令牌」（单测观察防刷屏闩的窗口，UI 用不到）。
    bool canRequestToken() const;

    // 控制端是否持有输入令牌（仅持有者可输入）。定义放 .cpp（同上）。
    bool hasToken() const;
    void releaseToken();  // 主动释放输入令牌（转发给 ClientConnection）

    // 断线自动重连的两个可观测状态（界面与测试都用它们判断）。
    // 前者是**全局偏好**（一处开关、对以后新开的每一格都生效）；
    // 后者按**选中格**回答 ——「在等重连吗」这个问题只有对某一条连接才成立。
    bool autoReconnectEnabled() const { return autoReconnectOn_; }
    bool reconnectPending() const;       // 选中格是否已排上一次重连（在等退避计时到点）

    // 程序化开关「断线自动重连」。UI 上唯一的编辑处是
    // 【设置…】对话框（开关控件不在主窗口里了），这条是与 setHostCodec /
    // setReceiveDir 同类的自测 / 单测入口。
    void setAutoReconnectEnabled(bool on);

    // 日志端口（测试与程序内部读日志的唯一入口）。
    // 界面上的日志区已按需求删除，日志改为落盘 txt；端口是内存缓冲 + 文件的收口。
    const lanrdp::LogPort *logPort() const { return logPort_.get(); }
    lanrdp::LogPort       *logPort() { return logPort_.get(); }

    // ---- 角色查询（两个角色可同时在线，这里只是「谁在跑」的事实）----
    //
    // 没有全局「模式」状态：被控 = isHosting()（是否正在监听），控制 = 任意一格
    // 有会话在跑。它们互相独立 —— 一边被别人连着、一边连别人是合法状态，
    // 剪贴板 / 状态栏统计在这种时刻以控制端为准（与 updateClipboardTarget 同一口径）。
    bool hostSessionActive() const;    // 正在监听
    bool clientSessionActive() const;  // 已建会话（含未握手）/ 正在等重连

    // ---- 视窗、悬浮条 ----
    //
    // 中央区是承载 1..4 个视窗的网格（`setCentralWidget()` 只能有一个，一控多必须
    // 换宿主）。下面这几个查询一律返回**当前选中那一格**的零件 —— 与「操作跟随选中」
    // 走的是同一套规则，测试与程序化入口因此不必自己算下标。
    lanrdp::ViewGrid *viewGrid() const { return grid_; }

    int                 viewCount() const { return views_.size(); }
    lanrdp::RemoteView *viewAt(int index) const;
    lanrdp::RemoteView *selectedView() const;

    lanrdp::ScreenOverlayBar *overlayBar() const;       // 选中格的条
    lanrdp::ScreenRenderer   *screenRenderer() const;   // 选中格的画面

    // 一控多的格数上限（最多 4 台）。
    static int maxViews();

    // 程序化设置「启用远程键鼠控制」总开关 —— 与【设置…】里勾选走**同一条**路径
    // （逐格注入给连接与渲染器 + 写日志）。测试与命令行自测用；正常交互不必调它。
    void setInputEnabledForTest(bool on);
    bool inputEnabled() const { return inputEnabled_; }

signals:
    void handshakeResult(bool ok, const QString &message);
    void frameReceived(const QSize &size, qint64 bytes);
    void listening(quint16 port);
    void listenFailed(quint16 port, const QString &reason);  // 监听起不来（端口被占等）
    void inputSent(int kind);      // 控制端发出一次输入事件
    void inputInjected(int kind);  // 被控端成功注入一次输入事件
    void captureFailed(const QString &error);  // 抓屏失败（锁屏 / 会话断开时必然发生）
    void fileSent(bool ok, const QString &path, const QString &message);      // 控制端发送结束
    void fileReceived(bool ok, const QString &path, const QString &message);  // 被控端接收结束
    void tokenStateChanged(bool hasToken);  // 控制端输入令牌状态变化
    void clientClosed();  // 控制端连接已关闭（见 halfCloseClientWhenFlushed）

private slots:
    void onDisconnectButtonClicked();
    void onSendFileClicked();
    void onClientListMenu(QToolButton *anchor);  // 条上「已连 N 个」的弹出菜单（踢出 / 收回操作权）
    void updateStats();

    // 网格里选中格变了 -> 更新每格的选中外观与条标题。
    // 注意它**不**改任何连接：选中只是「接下来的操作发给谁」。
    void onGridSelectionChanged(int index);

    // 被控端【收回操作权 / 恢复操作】双态菜单项（【被控】菜单里）。
    void onTakeInputButtonClicked();

    // 控制端自动重连开关（唯一编辑处是【设置…】）。
    void onAutoReconnectToggled(bool on);

protected:
    // 窗口尺寸变化后重新下发「显示预算」给**每一格**的解码器。
    // ⚠️ 一控多下预算取的是**格子尺寸**（每格自己的 renderer），所以切布局 / 占满
    // 也会改变预算 —— 那几条路径走 scheduleDisplayBudget()，不走这里
    // （窗口本身没变尺寸，收不到 resizeEvent）。
    void resizeEvent(QResizeEvent *event) override;

    // 首次显示时中央区尺寸才落定，补一次显示预算下发（幂等）。
    void showEvent(QShowEvent *event) override;

public slots:
    // 清空运行日志。改为只清**内存缓冲**，不动落盘文件。
    void clearLog();

private:
    void buildUi();
    void buildViews();                              // 建第 0 格（启动空态格）+ 网格接线
    lanrdp::RemoteView *addView();                  // 追加一格（新连接用）并接线
    // 「连接另一台」按钮的落地：只加一格**空视窗**（内显空态两入口），不弹对话框。
    // 到 4 格上限时只写日志说明，不加格。
    void addEmptyView();
    // 「这一格能不能拿来连下一台」——为空（还没连过）或那条连接已经死了才算能。
    // 活着 / 正在重连待命的那一格不复用（那属于「再开一格」的情形）。
    static bool viewIsReusable(const lanrdp::RemoteView *view);
    void buildHostMenu();            // 【被控】菜单（开始/停止监听 + 收回/恢复操作权）
    void buildViewMenu();            // 悬浮条显隐的「视图」菜单入口
    // 把【被控】菜单的两个双态项（监听开关、收回操作权）按当前状态刷新。
    void refreshHostMenu();

    // 把一格条上的控件接到本窗口（每格只接一次）。
    void wireView(lanrdp::RemoteView *view);
    // 把该格那条连接的信号接到本窗口与该格（日志 / 状态栏 / 按钮 /
    // 渲染 / 剪贴板 / 文件）。参数化之后对 N 格是同一份代码。
    void wireConnection(lanrdp::RemoteView *view);
    // 把该格画面的键鼠 / 拖拽接到**该格**的连接（不是「当前选中格」——
    // 事件自带来源，跟着来源走才对）。
    void wireRendererInput(lanrdp::RemoteView *view);

    // 该格的那条连接（可空）。
    lanrdp::ClientConnection *connectionOf(lanrdp::RemoteView *view) const;
    // 选中格的那条连接（可空）。**所有「跟随选中」的操作都收口到这里**，
    // 免得哪天有人忘了换成「按选中取」。
    lanrdp::ClientConnection *activeConnection() const;
    lanrdp::ScreenRenderer   *activeRenderer() const;

    // ---- 占满 / 还原 ----
    // 点某一格的【占满】：该格铺满、其余 `hide()`（**不销毁**，帧照收）。已经占满的
    // 再点一次 = 还原。入口是「按格」的 —— 点第 2 格的按钮就占满第 2 格，与选中无关。
    void toggleMaximize(lanrdp::RemoteView *view);
    // 占满态下按 Esc（RemoteView 在 renderer 上拦截并上报）。只对**正占满的那一格**
    // 生效 —— 其余格的 Esc 不是本地手势，本来就走远端。
    void restoreMaximize(lanrdp::RemoteView *view);
    // 把「谁是占满格 / 能不能占满」刷到每格的按钮与状态位上（权威状态在 ViewGrid，
    // RemoteView 只存镜像）。任何可能改变格数或占满索引的地方都要调它。
    void syncMaximizeState();

    // 把每格的**格子尺寸**下发给该格连接的解码器（作为解码输出上限框）。
    void pushDisplayBudget();
    // 格子尺寸变了（加格 / 占满 / 还原）之后重算显示预算。
    // ⚠️ 布局是**异步**落定的（invalidate 之后真正的几何要到下一轮事件循环才更新），
    // 当场读 `renderer->size()` 拿到的是旧值 ⇒ 同步来一次 + 排队再来一次。
    void scheduleDisplayBudget();

    // 只有一格可见（一格 / 占满）时，允许渲染器在死区也开平滑缩放。
    void updateSmoothDeadZone();

    void log(const QString &line);
    void setStatusText(const QString &text);

    // 对端断开 / 被踢 / 主动断开：复位界面（**只做 UI**，
    // 拆会话等内核清理已归 lanrdp::ClientConnection::teardown）。
    // 按**格**复位 —— 一控多时断开一台只该清它那一格。
    void handleClientDisconnected(lanrdp::RemoteView *view);

    // 按当前状态刷新条上按钮的可用性与文案（监听开关、断开、发送文件）。
    void updateConnectionUi();

    // UI 刷新：被控端「已连 N 个」按钮（含令牌持有标记）、控制端令牌状态。
    void refreshClientList();
    // 逐个视窗刷新操作权状态图标与【让出操作权】——每格看**自己那条连接**的
    // 令牌状态（一控多时各连各的令牌，不能看错格）。参数只为兼容既有的
    // `tokenStateChanged(bool)` 连接，实际不读。
    void updateTokenStatus(bool hasToken = false);

    // 机主收回远程输入权（三段式 lock -> release -> 通知）、解锁。
    void takeInput();
    void unlockInput();

    // 把「输入是否已锁定」同步到按钮文案与「已连 N 个」提示上。
    void updateHostTokenUi();

    // ---- 与宿主线程的交互（宿主在工作线程上，GUI 只能「投递」或「读快照」）----
    // 投递一份工作过去执行（fire-and-forget，Queued）。用 lambda 而不是字符串槽名：
    // 编译期检查，且不必为每个动作声明一个槽。
    void postToHost(std::function<void(lanrdp::HostController *)> work);
    // 只有这两件事需要「返回即已生效」⇒ 走阻塞投递（`Qt::BlockingQueuedConnection`）。
    // ⚠️ 前提是宿主线程**正在跑事件循环**；线程停了还投递会永久死锁。
    bool hostStartSync(quint16 port, const QString &password);
    void hostStopSync();

    // 剪贴板同步（仅文本）。单实例 + 动态切换同步目标。
    void setupClipboard();
    void updateClipboardTarget();

    // 三个对话框的入口（都被「是否已有会话」决定要不要先切模式）。
    void openHostConfigDialog();
    void openConnectDialog();
    void openSettingsDialog();

    // 带格参数的版本 —— 条上的【断开】【发送文件…】按格调用。
    // 无参的槽（onDisconnectButtonClicked / onSendFileClicked）保留给程序化入口与测试，
    // 内部就是转调选中格。
    void disconnectView(lanrdp::RemoteView *view);
    void sendFileFrom(lanrdp::RemoteView *view);

    // 断线格的两个出口（条上的【重连】/【关闭】）。
    // · retryView = 用记下的参数立刻再连一次（转发给 ClientConnection::reconnectNow）；
    // · closeView = 扔掉这一格；只剩一格时退化为「扔掉连接 + 复位成空态」
    // （中央区总得有个格，不能连格一起删）。
    void retryView(lanrdp::RemoteView *view);
    void closeView(lanrdp::RemoteView *view);

    // 这一格是不是「断线格」—— 有记下的对端、但此刻没连着。
    // 新建还没连过的连接对象 host 为空 ⇒ 不算断线（那是「未连接」空态）；
    // 正在等自动重连的**算** —— 用户看到的都是「画面停了」，两个出口对它同样适用。
    bool viewIsDisconnected(const lanrdp::RemoteView *view) const;

    // force=true 时忽略「启用远程键鼠控制」开关（命令行自测用）。
    void sendInputEvent(const lanrdp::InputEvent &event, bool force = false);

    // 逐格刷新：条上按钮显隐 / 条标题 / 空态层 / 断线格出口 —— 全部按**这一格自己的
    // 连接状态**判定（没有「模式」这个维度了），由 updateConnectionUi 驱动。
    void refreshViewUi();

    // ---- 视窗集合（一控多） ----
    //
    // 每格一个 `RemoteView`（画面 + 条 + 条上控件 + 该格连接），第 0 格在启动时就建好
    // ——它是启动时的空态格（没有连接对象），控制端每连一台加一格。
    //
    // 原来那批单份成员（bar_ / renderer_ / 9 个按钮 / connection_）随之下沉进
    // RemoteView：不是「换个地方存」，而是把「属于同一格」这件事变成一个对象 ——
    // 一控多时若改用平行数组，「谁跟谁是同一格」会散落在十几处下标里。
    QVector<lanrdp::RemoteView *> views_;

    // 日志端口（内存缓冲 + 落盘）。界面上已无日志区。
    // 用指针而非值成员：LogPort 内含 QMutex，不可拷贝/移动赋值。
    std::unique_ptr<lanrdp::LogPort> logPort_;

    QLabel       *statusLabel_  = nullptr;
    // 两端各一条传输进度条。落在**状态栏**（放条上会让条宽
    // 随文件名不断跳动）；常态隐藏，传输时出现，结束复位。
    QProgressBar *sendProgress_ = nullptr;
    QProgressBar *recvProgress_ = nullptr;

    // 空闲让出的兜底判定（5 s）与输入看门狗（200 ms 打点）都已随被控端逻辑
    // 一起搬进 HostController —— 它们服务的是「持有者令牌」与「注入通路」，
    // 与被控端同生共死。

    // 中央区宿主 = 多视窗网格（`setCentralWidget()` 只能有一个，
    // 而一控多要求中央区放 1..4 个视窗）。`views_` 的顺序即网格里的顺序。
    lanrdp::ViewGrid *grid_ = nullptr;

    // ⚠️ 格子没有主次：被控端的动作（监听开关 / 收回操作权）在菜单栏；
    // 「已连 N 个」状态在每格悬浮条最左（全局状态，逐格同刷）。

    // 被控端宿主：**一条工作线程 + 一个对象**。
    // 监听 / 会话池 / 抓屏编码 / 注入 / 文件落盘 / 令牌全在里面，本窗口只把它的信号
    // 呈现成界面；跨线程一律「投递 + 读快照」，绝不直接读宿主成员。
    //
    // ⚠️ 宿主对象用**裸指针**而不是 `unique_ptr`：它 `moveToThread` 之后归属工作线程，
    // 只能在**那个线程**里销毁（Qt 官方范式 = `QThread::finished` → `deleteLater`）。
    // 在主线程 `delete` 一个属于别的线程、且带着 QTimer 子对象的 QObject 是不安全的。
    //
    // ⚠️ 声明位置**必须晚于 `logPort_`**（见上）：宿主里有会写日志端口的线程，
    // 停机时（`~MainWindow` 里 `wait()` 之后）才轮到成员析构 ⇒ 端口必须活得久。
    QThread                *hostThread_ = nullptr;
    lanrdp::HostController *host_       = nullptr;

    // 宿主状态的**本地快照**，由 `uiStateChanged` 从工作线程推过来。
    // GUI 绝不直接去读工作线程的成员 —— 那是跨线程读。所有被控端 UI 都从这里取值。
    lanrdp::HostController::UiState hostState_;

    lanrdp::ClipboardSync *clipboardSync_ = nullptr;  // 单实例剪贴板同步（绑 QClipboard）

    bool inputEnabled_  = false;
    bool hostUseH264_   = true;
    int  hostBitrate_   = 8 * 1000 * 1000;

    // 原存在面板控件里的「启动参数」，面板删掉后改存成员 ——
    // 对话框每次开关都是新对象，参数得有地方活下来（下次打开还是上次填的值）。
    quint16 hostPort_      = 9000;
    QString hostPassword_;                              // 空 = 必须当场输入
    QString hostRecvDir_   = lanrdp::defaultReceiveDir();
    QString peerHost_      = QStringLiteral("127.0.0.1");
    quint16 peerPort_      = 9000;
    QString peerPassword_;                              // 空 = 必须当场输入

    // 上面这几个启动参数的落盘。路径为空 = 不持久化（见构造函数的说明）。
    lanrdp::SessionStore sessionStore_;

    // 【视图 → 悬浮操作条】的全局偏好。新加的格要继承它
    //（否则「收起条」之后一开新连接，条又冒出来了）。
    bool    barsHidden_ = false;

    // 「断线自动重连」是**全局偏好**（不再是某条连接的属性）。
    // 留一份在这里，是为了「以后新开的那一格也自动继承这个开关」——
    // 只存在连接里的话，新连接就只会拿到构造函数里的默认值。
    bool autoReconnectOn_ = true;

    // 统计（每秒刷新一次）。收帧**计数**已随连接搬进 ClientConnection
    // （它是「每条连接一份」的）；被控端侧的注入 / 发送计数随 `HostController`
    // 搬到工作线程，由 `uiStateChanged` 的快照推过来 ⇒ 这里只留**本窗口**的差分基准。
    QTimer  *statsTimer_    = nullptr;
    quint64  lastFrames_    = 0;
    quint64  lastBytes_     = 0;
    // 上一拍的**统计来源**（被控端 = streamer_，控制端 = 选中格那条连接）。
    // 换格 / 换角色时它变了 —— 上一来源的累计计数对新来源毫无意义，差值会无符号回绕。
    QObject *lastStatsSource_ = nullptr;
    // 帧自身上报的尺寸（状态栏分辨率口径，不依赖渲染器）。
    // **留在本窗口**：被控端抓屏与控制端收帧都会写它 —— 显示是 UI 关注点，
    // 不是某条连接的属性；一控多时再按「当前选中连接」取值。
    QSize    lastFrameSize_;
    QString  statsMode_;  // "host" / "client" / 空

    // 【被控】菜单的两个双态入口。被控端全局控件的家：动作进菜单栏
    // （开始/停止监听、收回/恢复操作权），状态进悬浮条（「已连 N 个」在每格条上最左，
    // 由 refreshClientList 逐格刷）。
    QAction    *listenAction_    = nullptr;   // 开始监听… / 停止监听
    QAction    *takeInputAction_ = nullptr;   // 收回操作权 / 恢复操作
};
