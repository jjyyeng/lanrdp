#pragma once

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

} // namespace lanrdp
