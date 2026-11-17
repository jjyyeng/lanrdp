#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

QT_BEGIN_NAMESPACE
class QTimer;
class QUdpSocket;
QT_END_NAMESPACE

namespace lanrdp {

// 局域网自动发现的实现：**UDP 广播**，与 TCP 侧毫无关系（不改协议版本）。
//
// 为什么是广播而不是组播：局域网里最常见的是「同一网段、随手就能互通」，
// 广播不需要任何组播路由配置；跨网段发现本来就不在目标里（工具是给同一间屋子的
// 两台机器用的）。
//
// 报文是**幂等的心跳**：每台在监听的机器每 3 秒广播一次「我是谁、我的 TCP 端口」，
// 收到就更新列表。没有请求 / 应答，因此没有状态机 —— 丢一个包不过晚 3 秒看到对方。
constexpr quint16 kDiscoveryPort = 9001;

// 广播间隔。
constexpr int kBeaconIntervalMs = 3000;

// 多久没听到就当作对方下线。取 3 个周期再多一点：正好在「丢两个包」时还不误删。
constexpr int kPeerExpiryMs = 10000;

// 机器名在报文里的字节上限（UTF-8）。超出截断 —— 名字只是给人看的。
constexpr int kMaxBeaconNameBytes = 64;

// 一条发现报文的内容。
struct Beacon
{
    quint32 instanceId = 0;   // 进程实例 id：用来把自己广播回来的包认出来
    quint16 port       = 0;   // 它正在监听的 TCP 端口（0 = 不合法，丢弃）
    QString name;             // 机器名
};

// 报文布局（小端）：'L''R''D''1' | u16 port | u32 instanceId | u8 nameLen | name
QByteArray encodeBeacon(const Beacon &beacon);

// 解不出合法报文（长度 / 魔数 / 端口为 0）时返回 nullopt。
// 端口为 0 必须拒：放过去的话列表里会出现一条点了连不上的条目。
std::optional<Beacon> decodeBeacon(const QByteArray &datagram);

// 从**一个**源地址听到过它。
//
// 一台机器同时接 Wi-Fi 和网线时，两块网卡都会把广播发出去，接收方看到的就是
// 「同一台机器、两个源地址」。这两个地址都要留着：它们各自都是一条能连过去的路，
// 而哪条快、哪条通只有用户知道（比如有线接内网、无线接外网时，只有其中一个能回来）。
struct PeerAddress
{
    QHostAddress address;
    // 上次从**这个**源地址听到它的时刻（Discovery 内部单调时钟的毫秒数）。
    // 过期判定按地址逐条算：拔掉一根网线只该消失那一行，不是整台机器。
    qint64       lastSeenMs = 0;
};

// 列表里的一项（一台机器）。
struct DiscoveredPeer
{
    quint32      instanceId = 0;
    QString      name;
    quint16      port = 0;
    // 全部已知源地址，**按地址数值升序**（顺序不该取决于 UDP 包谁先到 ——
    // 否则同一批机器每次刷新都换个顺序，看着就是在跳）。
    QList<PeerAddress> addresses;
    // 首选地址 = addresses 的第一条。给「只想填一个地址」的调用方用；
    // 界面要显示全部时请遍历 addresses。
    QHostAddress address;
};

// 广播目标（地址 + 端口）。生产环境按网卡算广播地址；单测指向 127.0.0.1。
struct BroadcastTarget
{
    QHostAddress address;
    quint16      port = kDiscoveryPort;
};

// 自动发现：既广播自己（在监听时），也收集别人。
//
// 一个实例一个对象，绑一个 UDP 端口。收不到包不是错误（防火墙挡广播很常见），
// 调用方应当把它当成「这次没自动找到，手输 IP 一样能连」。
class Discovery : public QObject
{
    Q_OBJECT

public:
    explicit Discovery(QObject *parent = nullptr);
    ~Discovery() override;

    // 绑定 UDP 端口并开始收包。port=0 让系统分配（单测用）。
    // 失败（端口被占 / 无网络栈）返回 false，原因见 lastError()。
    bool start(quint16 port = kDiscoveryPort);
    void stop();
    bool isRunning() const;

    // 绑定成功后的实际端口；未运行时为 0。
    quint16 listenPort() const;

    // 广播自己的 TCP 端口。0 = 停止广播（不再监听，但仍在收集别人的）。
    void setAdvertisePort(quint16 tcpPort);
    quint16 advertisePort() const;

    void setIntervalMs(int ms);
    void setExpiryMs(int ms);
    void setBroadcastTargets(const QList<BroadcastTarget> &targets);
    void sendBeaconNow();

    QList<DiscoveredPeer> peers() const;
    quint32 instanceId() const;

    QString lastError() const;

signals:
    void peersChanged();

private slots:
    void onReadyRead();
    void onTick();

private:
    void pruneExpired();
    QList<BroadcastTarget> targets() const;

    QUdpSocket *socket_ = nullptr;
    QTimer     *timer_  = nullptr;

    // 单调时钟：过期判定不能用墙钟（用户改系统时间就会把所有人误判成下线）。
    QElapsedTimer clock_;

    quint32 instanceId_ = 0;
    quint16 advertisePort_ = 0;
    quint16 listenPort_ = 0;
    int     intervalMs_ = kBeaconIntervalMs;
    int     expiryMs_ = kPeerExpiryMs;

    QList<BroadcastTarget> explicitTargets_;
    QList<DiscoveredPeer>  peers_;
    QString                lastError_;
};

} // namespace lanrdp
