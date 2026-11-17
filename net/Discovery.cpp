#include "net/Discovery.h"

#include "protocol/LE.h"

#include <QHostInfo>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>

namespace lanrdp {

namespace {

// 魔数带一个版本位（'L''R''D''1'）：将来改报文布局时把 1 改 2，
// 旧版本解不出新报文会直接丢弃 —— 广播包丢了只是晚 3 秒发现，代价可忽略。
constexpr char kBeaconMagic[4] = {'L', 'R', 'D', '1'};
constexpr int  kBeaconFixedSize = 4 + 2 + 4 + 1;   // 魔数 + port + id + nameLen

QString localDisplayName()
{
    QString name = QHostInfo::localHostName().trimmed();
    if (name.isEmpty())
        name = QStringLiteral("lanrdp");
    if (name.toUtf8().size() > kMaxBeaconNameBytes)
        name = QString::fromUtf8(name.toUtf8().left(kMaxBeaconNameBytes));
    return name;
}

} // namespace

QByteArray encodeBeacon(const Beacon &beacon)
{
    if (beacon.port == 0 || beacon.instanceId == 0)
        return {};

    const QByteArray name = beacon.name.toUtf8();
    if (name.size() > kMaxBeaconNameBytes)
        return {};

    QByteArray out;
    out.append(kBeaconMagic, 4);
    le::appendU16(out, beacon.port);
    le::appendU32(out, beacon.instanceId);
    le::appendU8(out, static_cast<quint8>(name.size()));
    out.append(name);
    return out;
}

std::optional<Beacon> decodeBeacon(const QByteArray &datagram)
{
    if (datagram.size() < kBeaconFixedSize || datagram.size() > 4096)
        return std::nullopt;
    if (datagram.left(4) != QByteArray(kBeaconMagic, 4))
        return std::nullopt;

    const quint16 port = le::readU16(datagram, 4);
    if (port == 0)
        return std::nullopt;   // 端口 0 连不上，放过去只会让列表里出现死条目

    Beacon out;
    out.port       = port;
    out.instanceId = le::readU32(datagram, 6);

    const int nameLen = static_cast<int>(le::readU8(datagram, 10));
    if (datagram.size() != kBeaconFixedSize + nameLen)
        return std::nullopt;   // 长度不自洽：布局对不上，丢弃而不是猜

    out.name = QString::fromUtf8(datagram.mid(kBeaconFixedSize, nameLen));
    if (out.instanceId == 0)
        return std::nullopt;
    return out;
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

Discovery::Discovery(QObject *parent)
    : QObject(parent)
{
    clock_.start();
    // 实例 id 只要「本进程唯一、重启后不会撞上别人的旧记录」就够，随机即可。
    // 0 留作「没有 id」的哨兵。
    do {
        instanceId_ = QRandomGenerator::global()->generate();
    } while (instanceId_ == 0);
}

Discovery::~Discovery()
{
    // 析构里不发信号：此时订阅者（对话框）可能已经先没了。
    if (timer_)
        timer_->stop();
    if (socket_) {
        socket_->close();
        delete socket_;
        socket_ = nullptr;
    }
}

bool Discovery::start(quint16 port)
{
    stop();

    socket_ = new QUdpSocket(this);
    // ShareAddress：同一台机器上双开（单实例关掉时）、或者本机既监听又探查时，
    // 两个 socket 绑同一个端口都还能收到广播。收不到也不会报错，只是少一条线索。
    if (!socket_->bind(QHostAddress::AnyIPv4, port,
                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        lastError_ = socket_->errorString();
        delete socket_;
        socket_ = nullptr;
        return false;
    }

    listenPort_ = socket_->localPort();
    connect(socket_, &QUdpSocket::readyRead, this, &Discovery::onReadyRead);

    timer_ = new QTimer(this);
    timer_->setInterval(intervalMs_);
    connect(timer_, &QTimer::timeout, this, &Discovery::onTick);
    timer_->start();

    lastError_.clear();
    return true;
}

void Discovery::stop()
{
    const bool wasRunning = socket_ != nullptr;

    if (timer_) {
        timer_->stop();
        timer_->deleteLater();
        timer_ = nullptr;
    }
    if (socket_) {
        socket_->close();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    listenPort_    = 0;
    advertisePort_ = 0;

    // 停了就听不到了，列表只会越来越旧 —— 直接清空，别让旧条目看起来还活着。
    if (wasRunning && !peers_.isEmpty()) {
        peers_.clear();
        emit peersChanged();
    }
}

bool Discovery::isRunning() const
{
    return socket_ != nullptr;
}

quint16 Discovery::listenPort() const
{
    return listenPort_;
}

void Discovery::setAdvertisePort(quint16 tcpPort)
{
    if (advertisePort_ == tcpPort)
        return;
    advertisePort_ = tcpPort;
    if (advertisePort_ != 0)
        sendBeaconNow();   // 立刻叫一声，不等下一个 3 秒周期
}

quint16 Discovery::advertisePort() const
{
    return advertisePort_;
}

void Discovery::setIntervalMs(int ms)
{
    intervalMs_ = ms > 0 ? ms : kBeaconIntervalMs;
    if (timer_)
        timer_->setInterval(intervalMs_);
}

void Discovery::setExpiryMs(int ms)
{
    expiryMs_ = ms > 0 ? ms : kPeerExpiryMs;
}

void Discovery::setBroadcastTargets(const QList<BroadcastTarget> &targets)
{
    explicitTargets_ = targets;
}

void Discovery::sendBeaconNow()
{
    if (!socket_ || advertisePort_ == 0)
        return;

    const QByteArray datagram = encodeBeacon(Beacon{instanceId_, advertisePort_,
                                                    localDisplayName()});
    if (datagram.isEmpty())
        return;

    for (const BroadcastTarget &target : targets())
        socket_->writeDatagram(datagram, target.address, target.port);
}

QList<DiscoveredPeer> Discovery::peers() const
{
    // 地址与条目的先后都不该取决于 UDP 包谁先到 —— 否则同一批机器每次刷新都换个
    // 顺序，看着就是在跳。地址按数值升序（同网段的自然挨在一起），
    // 机器按「名字 → 首选地址」。
    QList<DiscoveredPeer> out = peers_;
    for (DiscoveredPeer &peer : out) {
        std::sort(peer.addresses.begin(), peer.addresses.end(),
                  [](const PeerAddress &a, const PeerAddress &b) {
                      return a.address.toIPv4Address() < b.address.toIPv4Address();
                  });
        peer.address = peer.addresses.isEmpty() ? QHostAddress()
                                                : peer.addresses.first().address;
    }
    std::sort(out.begin(), out.end(), [](const DiscoveredPeer &a, const DiscoveredPeer &b) {
        const int byName = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        if (byName != 0)
            return byName < 0;
        return a.address.toIPv4Address() < b.address.toIPv4Address();
    });
    return out;
}

quint32 Discovery::instanceId() const
{
    return instanceId_;
}

QString Discovery::lastError() const
{
    return lastError_;
}

QList<BroadcastTarget> Discovery::targets() const
{
    if (!explicitTargets_.isEmpty())
        return explicitTargets_;

    QList<BroadcastTarget> out;
    const quint16 port = listenPort_ != 0 ? listenPort_ : kDiscoveryPort;

    // 全网广播 + 每块网卡的子网广播地址。
    // 后者是必须的：不少家用路由器 / 企业交换机会把 255.255.255.255 丢掉，
    // 而 192.168.1.255 反而能通。
    out.append(BroadcastTarget{QHostAddress::Broadcast, port});

    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            const QHostAddress bcast = entry.broadcast();
            if (!bcast.isNull())
                out.append(BroadcastTarget{bcast, port});
        }
    }
    return out;
}

void Discovery::onTick()
{
    sendBeaconNow();
    pruneExpired();
}

void Discovery::onReadyRead()
{
    if (!socket_)
        return;

    bool changed = false;
    while (socket_->hasPendingDatagrams()) {
        // pendingDatagramSize() 理论上不会为负，但为负时 resize() 会直接把缓冲区
        // 弄成非法状态 —— 一行守卫换「不崩」，值。
        const qint64 pending = socket_->pendingDatagramSize();
        if (pending <= 0)
            break;

        QByteArray buffer;
        buffer.resize(int(pending));
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 n = socket_->readDatagram(buffer.data(), buffer.size(), &sender,
                                               &senderPort);
        if (n <= 0)
            break;
        buffer.resize(int(n));

        const auto beacon = decodeBeacon(buffer);
        if (!beacon)
            continue;
        if (beacon->instanceId == instanceId_)
            continue;   // 自己广播回来的（广播会回灌到本机）

        const qint64 now = clock_.elapsed();

        auto it = std::find_if(peers_.begin(), peers_.end(),
                               [&beacon](const DiscoveredPeer &p) {
                                   return p.instanceId == beacon->instanceId;
                               });
        if (it == peers_.end()) {
            DiscoveredPeer peer;
            peer.instanceId = beacon->instanceId;
            peer.name       = beacon->name;
            peer.port       = beacon->port;
            peer.addresses.append(PeerAddress{sender, now});
            peer.address = sender;
            peers_.append(peer);
            changed = true;
        } else {
            // 端口 / 名字变了（同一台机器换了监听端口或改了名）要刷新界面。
            bool touched = false;
            if (it->port != beacon->port || it->name != beacon->name) {
                it->port = beacon->port;
                it->name = beacon->name;
                touched  = true;
            }

            // 地址按源地址累加，**不覆盖**：一台机器同时接 Wi-Fi 和网线时两个
            // 地址都会发广播，两条路都能连 —— 全都留着，界面上一条地址一行。
            // 只更新「这个地址还在」的时间戳，新地址才往集合里加。
            auto sameAddress = std::find_if(it->addresses.begin(), it->addresses.end(),
                                            [&sender](const PeerAddress &a) {
                                                return a.address == sender;
                                            });
            if (sameAddress == it->addresses.end()) {
                it->addresses.append(PeerAddress{sender, now});
                touched = true;
            } else {
                sameAddress->lastSeenMs = now;
            }

            if (touched)
                changed = true;
        }
    }

    if (changed)
        emit peersChanged();
}

void Discovery::pruneExpired()
{
    const qint64 now = clock_.elapsed();
    bool         changed = false;

    for (auto it = peers_.begin(); it != peers_.end();) {
        // 地址逐条过期：拔掉一根网线时该消失的只是那一行，不是整台机器。
        const int before = it->addresses.size();
        it->addresses.erase(std::remove_if(it->addresses.begin(), it->addresses.end(),
                                           [now, this](const PeerAddress &a) {
                                               return now - a.lastSeenMs > expiryMs_;
                                           }),
                            it->addresses.end());
        if (it->addresses.size() != before)
            changed = true;

        // 一个地址都不剩 = 真的听不到了（关掉了监听 / 进程退出）：
        // 留着一条点了连不上的记录，比没有自动发现更糟。
        if (it->addresses.isEmpty()) {
            it      = peers_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    if (changed)
        emit peersChanged();
}

} // namespace lanrdp
