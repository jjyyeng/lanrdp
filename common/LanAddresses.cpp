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

} // namespace lanrdp
