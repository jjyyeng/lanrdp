#pragma once

#include "protocol/Frame.h"
#include "protocol/InputEvent.h"

#include <QElapsedTimer>
#include <QObject>
#include <QSize>
#include <QString>

#include <cstdint>

class QImage;
class QTimer;

namespace lanrdp {

class AuthClient;
class DecoderWorker;
class FileSender;
class Session;

// 一条**控制端**连接的全部状态与生命周期。
//
// 来由：一控多（一个控制端连多台被控端）之前必须先能开多个连接实例 ——
// 而重构前 `MainWindow` 里控制端那组状态全是「唯一一份」语义（单个裸指针 `clientSession_`、
// 单份 decoder/FileSender/令牌状态/重连退避），且 `startConnect()` 第一句就是
// `teardownClient()`，所以「连第二台会先拆掉第一台」。本类把这些收拢成一个可多实例的对象。
// 与 `net/HostServer` 成对：**HostServer = 被控端的多会话**，**ClientConnection = 控制端的一条连接**。
//
// ⚠️ 边界：**只装「每条连接一份」的东西**。
// ✅ 进：`Session` / `AuthClient` / `DecoderWorker` / `FileSender` + 心跳定时器 + 重连定时器
// + 令牌状态（hasToken/tokenRequested）+ 退避计数 + 本次尝试计时 + 连接参数 + 收帧统计
// ❌ 不进：`ScreenRenderer`、进度条、按钮、剪贴板目标选择 —— 这些是 UI 与全局偏好，
// 不属于某条连接。通过信号 / setter 与上层交互。
// ⚠️ `inputEnabled_`（全局键鼠总开关）**本类自己也存一份**，但只是上层注入的**副本**：
// 真值在 `MainWindow`，变化时经 `setInputEnabled()` 同步过来（改开关要遍历所有连接）。
//
// 定时器**每连接一份**（不像被控端那样「统一定时器遍历 sessions_」）：
// 控制端的判死与退避天然按连接记，且连接数 ≤4，定时器开销可忽略。
//
// 日志：本类**不直接**调 MainWindow 的 log()，改发 `logMessage` 信号 ——
// 保持依赖方向单一（`net/` 不认识 `app/`）。
class ClientConnection : public QObject
{
    Q_OBJECT

public:
    explicit ClientConnection(QObject *parent = nullptr);
    ~ClientConnection() override;

    // 发起（重）连接；会先干净地拆掉上一轮。参数被记下供自动重连复用
    // （口令以前只活在输入框控件里，断线后无从取用）。
    //
    // `keepBackoff`：是否**保留**自动重连的退避计数。
    // 用户发起的新连接 / 手动重连传 false（默认）—— 退避从 1 倍基准重新起算；
    // 自动重连（onReconnectTimeout）必须传 true，否则每轮重连都把计数打回 0，
    // 退避退化成「永远 1 倍基准」。
    void connectTo(const QString &host, quint16 port, const QString &password,
                   bool keepBackoff = false);

    // 内核清理：停心跳 → 复位令牌 → 拆发送器/解码器/握手器/会话。
    // 刻意**不**停重连定时器 —— 断线清理紧接着就要靠它排下一次重连（见 teardown 注释）。
    // 幂等。
    void teardown();

    // 用户主动断开（界面【断开】）。语义 = 抑制重连 + 停退避 + 优雅关闭会话（若有）。
    // 会 emit `connectionLost()` 让上层复位界面 —— 含「重连待命、但已无活会话」这一种
    // （手工验证时捞出的「点【断开】界面不复位」bug）。
    void disconnectNow();

    // 用户点了某一格的【重连】—— 用记下的参数**立刻**再试一次
    // （顶掉在途退避、清零退避计数、清掉「用户主动断开」标记）。
    // 从没连过（没有记下的参数）时什么也不做。
    // 走的是与首次连接同一条路（`connectTo`），所以不需要第二套状态机。
    void reconnectNow();

    // ---- 控制端主动动作 ----
    // 发送文件。未就绪 / 文件不可读返回 false（FileSender 会再 emit 一次 fileFinished(false,…)）。
    bool sendFile(const QString &path);

    // **下一次**文件传输会用的 id。取值从随机数起步、每次递增 —— 它只保证
    // 「同一连接内不重复」，不保证全局唯一（接收侧已按来源隔离，不再依赖这点）。
    // 暴露出来是为了让「下一笔是几号」可被日志与测试观察到。
    quint32 nextTransferId() const { return nextTransferId_; }

    // 发送一次输入事件。未持令牌时**用这次操作去申请一次令牌**（幂等，见 requestToken）。
    // force=true 时忽略全局键鼠开关（命令行自测用）。
    void sendInput(const InputEvent &event, bool force = false);

    void requestToken();  // 幂等：已有在途请求不再发（防刷屏）
    void releaseToken();  // 主动释放（会通知对端）

    // ---- 状态查询（给 UI / 测试）----
    bool ready() const { return ready_; }               // 已握手
    bool connected() const { return session_ != nullptr; }  // 已建会话对象（可能仍在连 / 已连失败）
    bool hasToken() const { return hasToken_; }
    bool canRequestToken() const { return !tokenRequested_; }
    bool reconnectPending() const;
    Session *session() const { return session_; }

    // 死链判定（纯函数，便于不起真连接就钉住口径）。判据 = 对端静默超过了
    // deadTimeoutMs；静默的**计时起点**分两种：
    // · hasReceived：从「最后一次收到字节」起算（msSinceLastRx）；
    // · 一次都没收过（TCP 刚连上、握手还没回来）：从「本次尝试已过去多久」起算。
    // 后者不能用 msSinceLastRx —— 那时它还是「从未收到」哨兵，会让刚连上的
    // 连接在首个心跳 tick 就被判死。
    static bool isDeadLink(bool hasReceived, qint64 msSinceLastRx,
                           qint64 attemptElapsedMs, qint64 deadTimeoutMs);

    // ---- 外部注入的开关（不是连接的属性）----
    void setInputEnabled(bool on) { inputEnabled_ = on; }
    bool inputEnabled() const { return inputEnabled_; }

    // 上层（窗口）下发的「显示预算」= **这一格**的画面尺寸（**设备/物理像素**，
    // 不是逻辑像素 —— 见 MainWindow::pushDisplayBudget 的说明），作为解码输出的**尺寸上限框**。
    // 解码器会把 NV12 直接等比缩到这个框内（永不放大），省掉写全尺寸 ARGB32 的内存带宽。
    //
    // ⚠️ 立项时这里写的是「取窗口中央区，好让切换布局不用重新解码」——
    // **该理由已被实测推翻**（收益只取决于「输出面积 / 源面积」，中央区 ≈ 源的 79%
    // 等于 no-op）。改成**逐格**取，代价是换布局 / 占满之后要等一帧
    // 新尺寸的画面（由 MainWindow::scheduleDisplayBudget 排队重发）。
    //
    // ⚠️⚠️ 预算的**面积比要 ≲1/9** 才真的省（`nv12ToArgb32Scaled` 的收益守卫，阈值由
    // 压测 A2b 扫描定档）。别以为「给了预算就一定缩」：面积比 > 1/9 时它会退回全尺寸。
    // 两个必记的档位：4K 源 → 屏上 1/2 线性尺寸 = 面积比 **1/4**（慢一倍，最差点）；
    // 4K 屏上做 2×2 格子时单格正好落在这里 ⇒ **(1/9, 1/4] 是死区**（既省不下来，
    // 又要白付 QPainter 回退的 3.61 ms/帧）。死区怎么处置（夹尺寸？还是索性不给预算？）
    // **留到用 bench_decode_multi 实测后再定档，别凭感觉改**。
    //
    // 传 `QSize()`（无效）即关闭缩放。设早了也没关系：解码器是惰性创建的，
    // 这里只记下值，创建/重建时再下发。
    void setDisplayBudget(const QSize &size);
    QSize displayBudget() const { return displayBudget_; }
    // 解码线程重建的最小间隔（毫秒，默认 500）。
    //
    // 码流里的分辨率字段来自**对端数据**，可以被逐帧翻转；而重建解码线程是同步的
    // 重操作（停旧线程 + 起新线程 + 等 MFT 就绪，都在 UI 线程跑）。没有下限就等于
    // 把「逐帧重建」这条廉价 DoS 交到对端手里。测试里把它设成 0 或一个大值来观察
    // 「该重建时是否重建」与「抖动时是否被节流」。
    void setDecoderRebuildIntervalMs(int ms) { decoderRebuildIntervalMs_ = ms; }
    int  decoderRebuildIntervalMs() const { return decoderRebuildIntervalMs_; }
    // 注入「断线自动重连」偏好。**不**在此停下已有的退避定时器 ——
    // 关开关时上层应显式再调 `cancelReconnect()`（与旧 onAutoReconnectToggled 等价）。
    void setAutoReconnectEnabled(bool on) { autoReconnect_ = on; }
    bool autoReconnectEnabled() const { return autoReconnect_; }
    // 停掉在途退避并清零计数（关自动重连 / 用户断开时用）。
    void cancelReconnect();

    // 自动重连退避的**基准延迟**（默认 1000 ms；随后 ×2、×4… 由封顶值截住）。
    // 做成可配置是为了让测试把它压到毫秒级，不必真等 30 秒。
    void setReconnectBaseDelayMs(int ms) { reconnectBaseDelayMs_ = ms; }
    int  reconnectBaseDelayMs() const { return reconnectBaseDelayMs_; }

    // ---- 连接参数与收帧统计（给状态栏显示）----
    QString host() const { return lastHost_; }
    quint16 port() const { return lastPort_; }
    quint64 framesReceived() const { return rxFrames_; }
    quint64 bytesReceived() const { return rxBytes_; }

signals:
    void logMessage(const QString &line);       // 所有日志（含诊断）；上层决定落盘/显示
    void statusMessage(const QString &text);    // 状态栏文案
    void handshakeFinished(bool ok, const QString &message);  // 握手成功 / 被拒 / 未握手即断
    void readyChanged(bool ready);
    void tokenChanged(bool hasToken);
    void stateChanged();                        // 需要重刷按钮可用性
    // sourceSize = **码流真实尺寸**（不是 image.size()）。
    // image 可能是等比缩小后的（见 setDisplayBudget），状态栏分辨率
    // 与上层统计口径必须用 sourceSize，否则会显示成缩小后的小尺寸。
    void frameReady(const QImage &image, qint64 bytes, const QSize &sourceSize);
    void clipboardReceived(const lanrdp::Frame &frame);  // 交给上层的剪贴板同步器
    void connectionLost();                      // 连接断开 → 上层复位界面
    void inputSent(int kind);                   // 成功发出一帧输入

    // 文件发送进度（封装 FileSender，不把 FileSender 暴露给 app）
    void fileStarted(const QString &fileName, quint64 totalSize);
    void fileProgress(quint64 sent, quint64 total);
    void fileFinished(bool ok, const QString &path, const QString &message);

private slots:
    void onHeartbeatTick();
    void onReconnectTimeout();

private:
    void handleFrame(const Frame &frame);  // 收帧总入口（心跳 / 令牌 / 剪贴板 / 画面）
    void ensureFileSender();
    void scheduleReconnect();
    void onSocketDisconnected();

    Session      *session_    = nullptr;
    AuthClient   *authClient_ = nullptr;
    // 解码跑在**专属工作线程**里（DecoderWorker 持有 H264Decoder）。
    // 之前是 `H264Decoder *decoder_` 直接用 —— 4 路 4K 时把 UI 线程打到 125–155%，
    // 多视窗根本落不了地。惰性创建：首次收到 H.264 时按码流尺寸起一个 worker。
    DecoderWorker *decoder_   = nullptr;
    FileSender   *fileSender_ = nullptr;

    QTimer *heartbeatTimer_ = nullptr;  // 3 s：发心跳 + 判死
    QTimer *reconnectTimer_ = nullptr;  // singleShot：退避重连

    bool hasToken_               = false;
    bool tokenRequested_         = false;
    bool ready_                  = false;
    bool handshakeReported_      = false;  // 本次尝试是否已结算（旧 socket 迟到的错误不再冒泡）
    // 解码器打开失败的**连续次数**。不设一次性闩锁 —— 瞬态失败（如系统临时无
    // 解码器资源）会让画面永久黑屏。改为：只在**关键帧**上重试，连续失败到
    // kMaxDecoderOpenFailures 才彻底放弃（此后不再试，避免每帧都重建注定失败的 MFT）。
    int  decoderOpenFailures_    = 0;
    bool decoderUnavailable_     = false;  // 已判定「解码器在本机不可用」，不再重试
    // 上次**成功重建**解码线程的时刻（ms）。与 decoderRebuildIntervalMs_ 一起把
    // 「尺寸抖动 ⇒ 逐帧重建」挡住。
    int    decoderRebuildIntervalMs_ = 500;
    qint64 lastDecoderRebuildMs_     = 0;
    bool userInitiatedDisconnect_ = false;  // 用户主动断开 -> 抑制重连
    // 对端**明确拒绝了口令**。与「网络故障」必须分开：网断了该重连，口令错了
    // 重连一万次还是错 —— 只会每 30 s 刷一遍自己的日志，同时把被控端那侧的
    // 失败退避桶永远压在「刚失败过」的状态上。只在对方回了拒绝时才置位，
    // 由 connectTo() 在每次新的连接尝试时清掉（用户改了口令点【重连】也走这条路）。
    bool authRejected_            = false;
    bool inputEnabled_           = false;   // 全局键鼠开关（由上层注入）
    bool autoReconnect_          = true;
    QSize displayBudget_;                   // 解码输出尺寸上限框（由窗口下发）

    QString lastHost_;
    quint16 lastPort_ = 0;
    QString lastPassword_;
    int     reconnectAttempt_ = 0;
    int     reconnectBaseDelayMs_ = 1000;  // 退避基准（1 → 2 → 4 … s，测试可压到毫秒）
    QElapsedTimer attemptTimer_;  // 本次连接尝试已过去多久（判「连上却没握手」用）

    quint64 rxFrames_ = 0;
    quint64 rxBytes_  = 0;

    // 解码异步了，`frameDecoded` 回调里拿不到触发它的那个 frame。
    // 把「最近一次投递的 H.264 帧载荷字节数」记在这里供回调统计用
    // （口径 = 每产出一张图就加一次该帧 payload 字节）。
    qint64 pendingFrameBytes_ = 0;

    std::uint32_t nextTransferId_ = 1;
};

} // namespace lanrdp
