#pragma once

#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>

namespace lanrdp {

// 网卡名看起来是不是虚拟 / 隧道适配器（虚拟机、容器、VPN、加速器）。
//
// 这些地址列进「连我」卡片只会误导：对方从局域网根本连不到那张网卡。
// 只按名字判断，所以关键字必须**保守** —— 误杀一条真实网卡比漏杀一条虚拟网卡更糟
//（少了可用地址，用户会以为本机没有局域网地址）。
bool isVirtualInterfaceName(const QString &name);

// 本机可被局域网内其他机器访问的 IPv4 地址。
//
// 过滤：回环、链路本地（169.254/16）、未启用 / 未运行的网卡、虚拟适配器。
// 顺序保持系统枚举的顺序，不做排序 —— 用户看到的次序不该每天变。
QStringList lanAddresses();

// 卡片上那一行的文案：多个地址用「、」连接；一个都没有时说明怎么办。
//
// 空列表不是理论情况 —— 只插了网线但没拿到 IP、或全部网卡都是虚拟的时候就是这样。
QString lanAddressSummary(const QStringList &addresses);

// 复制到剪贴板的文本：每行一条 `ip:端口`，直接粘给对方就能用。
//
// 把拼接放在这里而不是在对话框里，是为了能脱离剪贴板单测 —— offscreen 平台上
// QClipboard 的行为不可靠，而这段文本的正确性比「能不能复制」更值得钉。
QString connectionCardText(const QStringList &addresses, quint16 port);

// 邀请串：对方拿着它就能把【新连接…】的三个框填对。
//
// 三行固定按 地址 / 端口 / 口令 排列，与对话框里从上到下的次序一致 ——
// 抄的时候不会串行，也不必猜哪一段是什么。
//
// 口令为空时**不留空行**（省掉口令那一行）：一段写着「口令」却什么都没有的文本，
// 会让人以为口令本来就可以空着，而空口令在对端几乎必被拒。
// 一个地址都没有时返回空串 —— 交给调用方把按钮禁掉，而不是产出一段残缺文本。
//
// 拼接放在这里而不是对话框里，理由同 connectionCardText：能脱离剪贴板单测
//（offscreen 平台上 QClipboard 不可靠）。
QString inviteText(const QStringList &addresses, quint16 port, const QString &password);

// ---- 入站来源过滤 ----
//
// 监听地址是 `0.0.0.0`：本机**每一块**网卡都在受理连接，包括 VPN 隧道、手机热点、
// 虚拟机桥接。这些对端与局域网里的对端在协议上完全一样，唯一能分辨的就是**来源地址**。
// 这里按「来源是否落在本机某块网卡所在的子网内」来判，而不是去绑某一块网卡的地址 ——
// 绑地址会连带砍掉另外几条真实可达的路（多网卡机器），也会挡掉 127.0.0.1 自测。
struct LocalSubnet
{
    QHostAddress network;          // 网络地址：主机位已按前缀长度清零
    int          prefixLength = 0;
};

// 本机各网卡所在的子网。
//
// 与 lanAddresses() 的口径**故意不同**：那里排除虚拟网卡，因为「把地址报给对方」时报
// 虚拟网卡只会误导；这里**不排除**，因为从 vEthernet / 隧道那侧真的可能有对端连进来
//（本机虚拟机、容器、VPN 对端），挡掉它们只会让人以为程序坏了。回环与未启用网卡仍排除。
QList<LocalSubnet> localSubnets();

// 对端地址是否可以受理：回环一律放行（单机双开、本机自测都指着它），其余必须落在
// 给定子网内。`subnets` 为空时只有回环放行 —— 一块可用网卡都没有，就没有「局域网」可言。
bool isPeerAllowed(const QHostAddress &peer, const QList<LocalSubnet> &subnets);

} // namespace lanrdp
