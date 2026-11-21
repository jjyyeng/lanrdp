#include "common/LanAddresses.h"

#include "common/I18n.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QNetworkInterface>

namespace lanrdp {
namespace {

// 关键字一律小写、保守。
//
// 刻意**不收**裸的 "tun"：三个字母太短，容易撞上真实网卡名（例如含 "Fortune" 之类）。
// 隧道网卡用 "tunnel" / "wireguard" / "openvpn" 这几个更具体的词覆盖。
const char *const kVirtualKeywords[] = {
    "vmware",       "virtualbox", "vbox",      "hyper-v",   "vethernet",
    "wsl",          "docker",     "tap-windows", "tunnel",  "wintun",
    "wireguard",    "openvpn",    "tailscale", "zerotier",  "hamachi",
    "radmin",       "npcap",      "loopback",  "uu accelerator",
};

// 把 IPv4 地址的主机位清零，得到它所在子网的网络地址。
quint32 maskIpv4(quint32 address, int prefixLength)
{
    if (prefixLength <= 0)
        return 0;
    if (prefixLength >= 32)
        return address;
    const quint32 mask = 0xFFFFFFFFu << (32 - prefixLength);
    return address & mask;
}

// IPv6 同理。逐字节搬，最后一个不完整的字节按位掩。
QHostAddress maskIpv6(const QHostAddress &address, int prefixLength)
{
    const QIPv6Address v6 = address.toIPv6Address();
    QIPv6Address       out{};
    for (int i = 0; i < 16; ++i) {
        const int bits = prefixLength - i * 8;
        if (bits >= 8) {
            out.c[i] = v6.c[i];
        } else if (bits > 0) {
            out.c[i] = static_cast<quint8>(v6.c[i] & (0xFFu << (8 - bits)));
        } else {
            out.c[i] = 0;
        }
    }
    return QHostAddress(out);
}

bool sameIpv6Prefix(const QHostAddress &a, const QHostAddress &b, int prefixLength)
{
    const QIPv6Address va = a.toIPv6Address();
    const QIPv6Address vb = b.toIPv6Address();
    for (int i = 0; i < 16; ++i) {
        const int bits = prefixLength - i * 8;
        if (bits >= 8) {
            if (va.c[i] != vb.c[i])
                return false;
        } else if (bits > 0) {
            const quint8 mask = static_cast<quint8>(0xFFu << (8 - bits));
            if ((va.c[i] & mask) != (vb.c[i] & mask))
                return false;
            return true;
        } else {
            return true;
        }
    }
    return true;
}

} // namespace

bool isVirtualInterfaceName(const QString &name)
{
    const QString lower = name.toLower();
    for (const char *keyword : kVirtualKeywords) {
        if (lower.contains(QLatin1String(keyword)))
            return true;
    }
    return false;
}

QStringList lanAddresses()
{
    QStringList out;

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        // 未启用 / 未运行的网卡即使有地址也连不通。
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning))
            continue;
        if (flags & QNetworkInterface::IsLoopBack)
            continue;
        if (isVirtualInterfaceName(iface.humanReadableName())
            || isVirtualInterfaceName(iface.name()))
            continue;

        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (ip.isLoopback() || ip.isLinkLocal())
                continue;

            const QString text = ip.toString();
            if (!out.contains(text))   // 同一地址可能挂在多张网卡上（桥接）。
                out.append(text);
        }
    }
    return out;
}

QString lanAddressSummary(const QStringList &addresses)
{
    if (addresses.isEmpty())
        return QCoreApplication::translate("lanrdp",
                                           "没有找到可用的局域网地址：请确认已连上网络。");

    // 顿号是中文标点，英文界面下要换成逗号 —— 分隔符也随界面语言走，
    // 与上面那句提示同一个判据（否则英文界面里会夹着一串中文顿号）。
    const QString sep = currentUiLanguage() == UiLanguage::Chinese
        ? QStringLiteral("、")
        : QStringLiteral(", ");
    return addresses.join(sep);
}

QString connectionCardText(const QStringList &addresses, quint16 port)
{
    QStringList lines;
    lines.reserve(addresses.size());
    for (const QString &address : addresses)
        lines << QStringLiteral("%1:%2").arg(address).arg(port);
    return lines.join(QLatin1Char('\n'));
}

QString inviteText(const QStringList &addresses, quint16 port, const QString &password)
{
    if (addresses.isEmpty())
        return QString();

    // 自由函数的文案不会被 lupdate 自动收走，必须写成带 "lanrdp" 上下文的字面量。
    QStringList lines;
    lines << QCoreApplication::translate("lanrdp", "地址 %1")
                 .arg(addresses.join(QStringLiteral("、")));
    lines << QCoreApplication::translate("lanrdp", "端口 %1").arg(port);
    if (!password.isEmpty())
        lines << QCoreApplication::translate("lanrdp", "口令 %1").arg(password);
    return lines.join(QLatin1Char('\n'));
}

QList<LocalSubnet> localSubnets()
{
    QList<LocalSubnet> out;

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        // 未启用 / 未运行的网卡：连本机自己都走不通，它的子网自然不必受理。
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning))
            continue;
        if (flags & QNetworkInterface::IsLoopBack)
            continue;   // 回环由 isPeerAllowed 单独放行，不必占一条子网

        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress ip      = entry.ip();
            const QHostAddress netmask = entry.netmask();
            if (ip.isNull())
                continue;
            if (ip.protocol() == QAbstractSocket::IPv4Protocol) {
                bool            ok      = false;
                const quint32   addr    = ip.toIPv4Address(&ok);
                const quint32   mask    = netmask.toIPv4Address();
                if (!ok)
                    continue;
                // 前缀长度优先用系统给的；老接口不给时按掩码位数算。
                int prefix = entry.prefixLength();
                if (prefix <= 0 || prefix > 32) {
                    prefix = 0;
                    for (quint32 m = mask; m != 0; m <<= 1)
                        ++prefix;
                }
                const quint32 network = maskIpv4(addr, prefix);
                // 同一地址可能挂在多张网卡上（Windows 桥接），别重复收录。
                bool dup = false;
                for (const LocalSubnet &s : out) {
                    if (s.prefixLength == prefix && s.network.toIPv4Address() == network) {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                    out.append(LocalSubnet{QHostAddress(network), prefix});
                continue;
            }

            if (ip.protocol() == QAbstractSocket::IPv6Protocol) {
                const int prefix = entry.prefixLength();
                if (prefix <= 0 || prefix > 128)
                    continue;   // 没给前缀长度就没法定义「子网」
                out.append(LocalSubnet{QHostAddress(maskIpv6(ip, prefix)), prefix});
            }
        }
    }
    return out;
}

bool isPeerAllowed(const QHostAddress &peer, const QList<LocalSubnet> &subnets)
{
    if (peer.isNull())
        return false;

    // 回环两种形态都算：v4 的 127/8、v6 的 ::1。单机双开、本机自测全靠它。
    if (peer.isLoopback())
        return true;

    // 双栈监听下对端常常是 `::ffff:a.b.c.d`（见 sourceIpKey 的注释）——
    // 不归一化的话 IPv4 子网永远匹配不上，表现为「同网段的机器全被挡」。
    bool          ok   = false;
    const quint32 peer4 = peer.toIPv4Address(&ok);

    for (const LocalSubnet &subnet : subnets) {
        // 前缀 0（0.0.0.0/0、::/0）在语义上匹配一切，交下面按族处理即可；
        // 这里只挡掉明显无效的值。
        if (subnet.prefixLength < 0 || subnet.prefixLength > 128)
            continue;

        if (ok && subnet.network.protocol() == QAbstractSocket::IPv4Protocol) {
            if (subnet.prefixLength > 32)
                continue;
            if (maskIpv4(peer4, subnet.prefixLength)
                == subnet.network.toIPv4Address())
                return true;
            continue;
        }
        if (!ok && subnet.network.protocol() == QAbstractSocket::IPv6Protocol) {
            if (sameIpv6Prefix(peer, subnet.network, subnet.prefixLength))
                return true;
        }
    }
    return false;
}

} // namespace lanrdp
