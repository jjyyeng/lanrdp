#pragma once

#include "host/TokenGate.h"
#include "protocol/Frame.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include <unordered_map>

class QTimer;

namespace lanrdp {

class FileReceiver;
class HostServer;
class LogPort;
class ScreenStreamer;
class Session;

// 被控端宿主：监听 + 会话池 + 抓屏编码 + 输入注入 + 文件接收 + 输入令牌。
//
// 从前这一整套**内联在 `app/MainWindow`**里（与菜单 / 对话框 / 状态栏挤在同一个类），
// 后果是「被控链路 = GUI 线程」这件事没有任何边界可循。本类把边界划出来：
// **对外只给信号与查询，不暴露任何内部对象指针** —— 这样日后要把整个对象
// `moveToThread` 搬到工作线程时，调用方一行都不用改。
//
// 本类**不依赖任何 QWidget / QClipboard**（因此可以进 `lanrdp_core`）：
// · 本机预览帧以 `previewFrame()` 送出去，画不画由 UI 决定；
// · 剪贴板只做「这一帧的发送者是否持令牌」的判定，正文原样交给 UI 侧写系统剪贴板。
class HostController : public QObject
{
    Q_OBJECT

public:
    // 一个已入池控制端的只读快照（UI 画「已连 N 个」列表用）。
    // 给快照而不是给 Session* 列表，是为了让 UI 不必自己算「谁持令牌」。
    struct ClientInfo
    {
        // 会话的单调标识，**不是指针**：那条会话可能已经释放、地址又被新连接复用。
        // 按 id 回查最坏是 no-op，按指针回查会踢错人。
        quint64 sessionId = 0;
        QString peer;               // "ip:port"
        bool    isHolder = false;   // 当前是否持输入令牌
    };

    // 给 UI 的**只读快照**，按值跨线程送过去。
    //
    // 为什么要这么一坨：本对象将来跑在工作线程上，**GUI 侧一次都不能去读它的成员**
    //（那是跨线程读，UB）。所以把 UI 会查的一切（状态 + 列表 + 统计）打包成一个值，
    // 由工作线程在状态变化时推过去；GUI 只读自己那份缓存。
    struct UiState
    {
        bool                listening   = false;
        bool                inputLocked = false;
        int                 clientCount = 0;
        QVector<ClientInfo> clients;

        quint64 framesSent        = 0;
        quint64 bytesSent         = 0;
        quint64 injectedCount     = 0;
        quint64 injectFailedCount = 0;
    };

    // logPort 可为空（则不记日志）。生命周期由调用方保证：**必须活得比本对象久**。
    explicit HostController(LogPort *logPort = nullptr, QObject *parent = nullptr);
    ~HostController() override;

    // 开始监听。port = 0 表示由系统分配临时端口（测试用）。
    // 已监听时先干净地停掉上一轮（与从前「重复调用 startHost」的语义一致）。
    // 返回 false = 起不来，原因已由 listenFailed() 给出。
    bool start(quint16 port, const QString &password);

    // 停止监听并断开全部控制端（窗口保留，可再次 start()）。幂等。
    void stop();

    bool    isListening() const;
    quint16 port() const;

    // 编码方式（用户**意图**；H.264 打不开时 ScreenStreamer 会自行降级为裸帧）。
    void setCodec(bool useH264, int bitrateBps = 8 * 1000 * 1000);
    bool usesH264() const { return useH264_; }
    int  bitrate() const { return bitrate_; }

    // 接收文件的落盘目录。调用方负责在 start() 之前给一个非空默认值
    // ——本类刻意不内建默认值，免得又引入一个「相对路径按进程 CWD 解析」的坑。
    void    setReceiveDir(const QString &dir);
    QString receiveDir() const { return receiveDir_; }

    // 本机预览开关。**关闭时根本不 emit `previewFrame`** —— 过滤必须留在这一侧：
    // 跨线程投递下，GUI 若被模态循环占住，队列会积压（4K 每帧独立缓冲 ≈33 MB，
    // 20 s 就够上 GB）。`frameSizeChanged` 仍无条件发（状态栏要用）。
    void setPreviewEnabled(bool on) { previewEnabled_ = on; }
    bool previewEnabled() const { return previewEnabled_; }

    // 剪贴板（UI 侧调，投递到本线程执行）：把已编码好的帧发给当前持令牌的控制端。
    // UI 不能自己做这件事 —— 它手里既没有可靠的持有者指针，也不能碰工作线程的 socket。
    void sendClipboardFrame(const Frame &frame);

    QVector<ClientInfo> clients() const;
    int  clientCount() const;
    void dropClient(quint64 sessionId);
    void dropAllClients();

    // ---- 输入令牌 ----
    bool     inputLocked() const;
    Session *holderSession() const;
    void     takeInput();    // 三段式 lock -> release -> 通知对端（顺序不能反）
    void     unlockInput();

    // ---- 统计与诊断（状态栏 / 日志口径）----
    quint64 framesSent() const;
    quint64 bytesSent() const;
    quint64 injectedCount() const { return injectedCount_; }
    quint64 injectFailedCount() const { return injectFailedCount_; }
    QString sourceInfo() const;
    QString codecInfo() const;

signals:
    void listening(quint16 port);
    void listenFailed(quint16 port, const QString &reason);

    // 日志 / 状态栏文案由 UI 决定怎么呈现（落盘、状态栏、stderr…）。
    // 本类自己只往 logPort 写文件，不碰任何界面。
    void logLine(const QString &line);
    void statusText(const QString &text);

    // UI 快照：状态 / 列表 / 统计任何一处变化时推给 GUI（跨线程自动排队）。
    // 取代了原来的 `clientListChanged()` —— 列表也是快照的一部分，不必单独发。
    // ⚠️ 「令牌易主」也走这条：列表与剪贴板目标本来就该同一时刻刷新。
    void uiStateChanged(const lanrdp::HostController::UiState &state);

    void previewFrame(const QImage &frame);    // 本机预览帧（UI 决定画不画）
    void frameSizeChanged(const QSize &size);  // 状态栏分辨率口径

    // 接收文件：只报事件与数字，文案 / 进度条由 UI 侧做。
    void fileStarted(const QString &name, quint64 total);
    void fileProgress(quint64 received, quint64 total);
    void fileReceived(bool ok, const QString &path, const QString &message);

    void inputInjected(int kind);  // 成功注入一次（状态栏计数用）
    void captureFailed(const QString &error);

    // 收到**持令牌者**发来的剪贴板帧（原样转发；写系统剪贴板由 UI 侧做，
    // 因为 `QClipboard` 只能在 GUI 线程碰）。
    void clipboardFrameReceived(const Frame &frame);

private:
    // TokenGate 的身份是「绝不复用」的单调 id，而调用方手上只有 Session*。
    // 这里惰性分配 + 反向查表；⚠️ 会话销毁时必须 erase，否则新会话落在同一地址
    // 会「继承」旧持有者的令牌。
    TokenGate::Token tokenOf(Session *s);
    Session         *sessionOfToken(TokenGate::Token t) const;
    // 纯读版：不改身份表，供 clients() 这类查询用。
    bool isHolderOf(Session *s) const;

    // 会话事件（都是 HostServer / Session 的信号出口）。
    void handleAuthenticated(Session *session);
    void handleRejected(Session *session);
    void handleDisconnected(Session *session);
    void handleSessionFrame(Session *session, const Frame &frame);

    // 幂等：在当前线程里建立内部定时器（只从 start() 调）。
    void ensureTimers();
    // 令牌因「空闲超时」被让出后的统一善后：通知本人 + 补发松开 + 记一行日志。
    // 两条触发路径共用（低频定时器 / 有人来抢时的惰性判定）—— 措辞分叉过，
    // 其中一条曾经完全没通知被过期者，对方界面一直显示握着操作权。
    void announceIdleRelease(TokenGate::Token expired);
    // 若我们注入过「按下」却还没注入过「抬起」，就补一针松开。
    // 调用时机：连接断开 / 收回令牌 / 停止监听 —— 那些「对端的抬起再也送不到」的时刻。
    void releaseStuckMouseButtons(const QString &reason);
    // 当前状态的快照 + 推给 UI。任何会改变状态的路径收尾都要调一次 notifyUi()。
    UiState uiState() const;
    void    notifyUi();
    void applyCodec();
    void ensureFileReceiver();
    QRect targetMonitorRect() const;
    void  onIdleCheck();

    LogPort *logPort_ = nullptr;   // 不持有

    HostServer     *host_         = nullptr;
    ScreenStreamer *streamer_     = nullptr;
    FileReceiver   *fileReceiver_ = nullptr;

    TokenGate tokenGate_;
    std::unordered_map<Session *, TokenGate::Token> tokenIds_;

    QTimer *idleTimer_    = nullptr;  // 5 s：空闲持有者的兜底判定
    QTimer *statsTimer_   = nullptr;  // 1 s：把统计推给 UI（跨线程不能按需读取）

    bool previewEnabled_ = false;     // 见 setPreviewEnabled()

    QString receiveDir_;
    bool    useH264_ = true;
    int     bitrate_ = 8 * 1000 * 1000;

    // 逻辑按下的鼠标键掩码（位 0 左 / 位 1 右 / 位 2 中）。
    // 「逻辑」= 我们**注入过按下、还没注入过抬起**，与物理键无关。
    // 只服务于 releaseStuckMouseButtons()：净身出户时若掩码非 0，就说明对端还欠我们一针抬起。
    quint32 mouseButtonsDown_  = 0;
    quint64 injectedCount_     = 0;
    quint64 injectFailedCount_ = 0;
};

} // namespace lanrdp

// 跨线程排队投递要求 Qt 认识这个类型（Qt 6 会据此自动注册）。
Q_DECLARE_METATYPE(lanrdp::HostController::UiState)
