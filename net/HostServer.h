#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QVector>

#include "net/AuthBackoff.h"

class QTcpServer;
class QTimer;

namespace lanrdp {

class Session;

// 被控端监听器：accept 每条入站连接 -> 建立 Session -> 跑鉴权握手。
// 通过者进入 sessions_（向这些会话推送画面）。
//
// 登录失败退避：认证比对失败的来源会被按 IP 记账，未到点的新连接
// 在 onNewConnection() 里**直接回绝、连 Session 都不建**。
class HostServer : public QObject
{
    Q_OBJECT

public:
    explicit HostServer(QString password, QObject *parent = nullptr);

    // port 传 0 表示由系统分配临时端口（测试用）。
    //
    // 已监听时：请求的端口/地址与当前**一致**就当作幂等成功；**不一致**则先停掉旧的
    // 再按新参数监听 —— 否则调用方（改了端口后重新监听）以为换了端口，实际仍在旧端口上，
    // 表现为「旧端口还能连、新端口连不上」，极易被误判成防火墙问题。
    bool listen(quint16 port, const QHostAddress &address = QHostAddress::Any);
    void stop();

    bool    isListening() const;
    quint16 port() const;

    // 已鉴权会话数上限（默认 4）。超出后新的鉴权请求在回 OK 前被拒。
    void setMaxClients(int max) { maxClients_ = max; }
    int  maxClients() const { return maxClients_; }

    QVector<Session *> sessions() const { return sessions_; }

    // 踢出控制端。dropClient 按**会话 id** 断开指定会话（id 不在池中则安全 no-op）；
    // dropAllClients 断开全部但**保持监听**，之后新控制端仍可接入。
    void dropClient(quint64 sessionId);
    void dropAllClients();

    // 心跳与死链判定。
    // heartbeatIntervalMs —— 多久给每个会话发一次 Heartbeat（默认 3000）；
    // deadTimeoutMs —— 多久没收到该会话的任何字节就判死并踢掉（默认 10000）。
    // ⚠️ 只有 `heartbeatIntervalMs` 必须在 `listen()` **之前**设置（它在 listen 成功时被
    // 写进心跳定时器的 interval）；`deadTimeoutMs` 是**每拍现读**的 ⇒ listen 之后改同样生效。
    // 测试里会压成 50 / 150 ms，所以做成可配置而不是写死常量。
    static constexpr int kDefaultHeartbeatIntervalMs = 3000;
    static constexpr int kDefaultDeadTimeoutMs      = 10000;

    void setHeartbeatIntervalMs(int ms) { heartbeatIntervalMs_ = ms; }
    int  heartbeatIntervalMs() const { return heartbeatIntervalMs_; }
    void setDeadTimeoutMs(int ms) { deadTimeoutMs_ = ms; }
    int  deadTimeoutMs() const { return deadTimeoutMs_; }

    // 登录失败退避配置（参数与时间源都可注入，详见 AuthBackoff）。
    // 测试里把 base/cap/窗口压到毫秒级，不真等 15 分钟。
    void setBackoffConfig(const AuthBackoff::Config &config) { backoff_ = AuthBackoff(config); }
    const AuthBackoff &backoff() const { return backoff_; }

signals:
    void listening(quint16 port);
    // listen() 失败时给出**可行动**的原因（端口被占用时直接说清「换个端口 / 关掉占用者」）。
    // 单看 QTcpServer::errorString() 只会说 "The address is already in use"，用户不知道该关谁。
    void listenFailed(quint16 port, const QString &reason);
    void clientAuthenticated(lanrdp::Session *session);
    void clientRejected(lanrdp::Session *session);
    void clientDisconnected(lanrdp::Session *session);
    // 新连接因失败退避被直接挡下（尚未建 Session）。waitMs = 还需等多久。
    void connectionThrottled(const QString &sourceIp, qint64 waitMs);

private slots:
    void onNewConnection();
    void onHeartbeatTick();

private:
    // 会话离开池的唯一出口：移出 sessions_ + 发 clientDisconnected + deleteLater。
    // 幂等——重复调用不会重复发信号。
    void removeSession(Session *session);

    // 按会话指针踢人。只给**本线程内部**用（心跳判死、批量断开）：
    // 池里的指针本来就由本类持有，不存在 ABA。对外一律走按 id 的 dropClient。
    void dropSession(Session *session);

    // 判死专用：直接 abort，不等对端 FIN。
    // 对端已经不回字节了，`disconnectFromHost` 会卡在 ClosingState 等 TCP 重传
    // 超时（Windows 默认十几秒）才真正回收，期间一直占着 maxClients_ 名额。
    // 对外踢人（dropClient）仍走优雅断开 —— 那种场景对端还活着，值得让它收到 FIN。
    void abortSession(Session *session);

    QTcpServer       *server_ = nullptr;
    QTimer           *heartbeatTimer_ = nullptr;
    QString           password_;
    QVector<Session *> sessions_;
    int               maxClients_ = 4;
    int               heartbeatIntervalMs_ = kDefaultHeartbeatIntervalMs;
    int               deadTimeoutMs_       = kDefaultDeadTimeoutMs;
    AuthBackoff       backoff_;
};

} // namespace lanrdp
