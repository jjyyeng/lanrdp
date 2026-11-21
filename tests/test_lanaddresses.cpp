#include <QtTest>

#include <QHostAddress>

#include "common/LanAddresses.h"

// 「连我」卡片列出来的地址必须是**对方真能连上**的那些。
//
// 本机同时有有线、无线、UU 加速器虚拟网卡、VPN 隧道是常态（项目的环境笔记里就记着
// 三条路）。把所有地址一股脑列出来，用户挑中虚拟网卡那条的概率不低，而症状是
// 「一直连不上」—— 比什么都不给更糟。
class TestLanAddresses : public QObject
{
    Q_OBJECT

private slots:
    void virtualAdaptersAreRecognised();
    void physicalAdaptersAreNotMisjudged();
    void lanAddressesExcludeLoopback();
    void summaryJoinsAddresses();
    void summaryExplainsWhenEmpty();
    void cardTextIsOneEndpointPerLine();
    void inviteTextCarriesTheThreeFields();
    void inviteTextOmitsThePasswordLineWhenEmpty();
    void inviteTextIsEmptyWithoutAddresses();

    // 入站来源过滤：0.0.0.0 会把 VPN / 热点 / 虚拟机桥接上的对端一并受理，
    // 能分辨的只有来源地址。
    void peerInsideLocalSubnetIsAllowed();
    void peerOutsideSubnetIsRejected();
    void peerLoopbackIsAlwaysAllowed();
    void peerMatchesAnyOfSeveralSubnets();
    void peerMappedIpv4StillMatchesSubnets();
    void peerIpv6UsesThePrefix();
    void peerWithoutAnySubnetAllowsOnlyLoopback();
};

void TestLanAddresses::virtualAdaptersAreRecognised()
{
    // 虚拟机 / 容器 / 隧道 / 加速器：这些地址对面连不到。
    const QStringList virtualNames = {
        QStringLiteral("VMware Network Adapter VMnet1"),
        QStringLiteral("VirtualBox Host-Only Network"),
        QStringLiteral("vEthernet (Default Switch)"),
        QStringLiteral("Hyper-V Virtual Ethernet Adapter"),
        QStringLiteral("vEthernet (WSL)"),
        QStringLiteral("TAP-Windows Adapter V9"),
        QStringLiteral("WireGuard Tunnel"),
        QStringLiteral("Tailscale"),
        QStringLiteral("ZeroTier One [1.2.3.4]"),
        QStringLiteral("Npcap Loopback Adapter"),
        QStringLiteral("uu accelerator virtual adapter"),
    };
    for (const QString &name : virtualNames) {
        QVERIFY2(lanrdp::isVirtualInterfaceName(name), qPrintable(name));
    }
}

// 真实的物理网卡不能被误杀：漏掉一条真网卡，卡片上就少了一个可用地址，
// 用户会以为「本机没有局域网地址」。
void TestLanAddresses::physicalAdaptersAreNotMisjudged()
{
    const QStringList physicalNames = {
        QStringLiteral("以太网"),
        QStringLiteral("WLAN"),
        QStringLiteral("Realtek PCIe GbE Family Controller"),
        QStringLiteral("Intel(R) Wi-Fi 6 AX201 160MHz"),
        QStringLiteral("Ethernet 2"),
    };
    for (const QString &name : physicalNames) {
        QVERIFY2(!lanrdp::isVirtualInterfaceName(name), qPrintable(name));
    }
}

// 本机真实调用：至少不能把回环地址列进去 —— 那是最没用的一个。
void TestLanAddresses::lanAddressesExcludeLoopback()
{
    const QStringList addrs = lanrdp::lanAddresses();
    for (const QString &a : addrs) {
        QVERIFY2(a != QStringLiteral("127.0.0.1"), qPrintable(a));
        QVERIFY2(!a.startsWith(QStringLiteral("169.254.")),
                 qPrintable(QStringLiteral("链路本地地址连不上：") + a));
    }
}

void TestLanAddresses::summaryJoinsAddresses()
{
    const QStringList addrs = {QStringLiteral("192.168.1.104"), QStringLiteral("192.168.11.3")};
    QCOMPARE(lanrdp::lanAddressSummary(addrs),
             QStringLiteral("192.168.1.104、192.168.11.3"));
}

// 一个地址都没有是真实情况（网线插着但没拿到 IP、或网卡全是虚拟的）。
// 此时不能显示空行 —— 那看起来像界面坏了。
void TestLanAddresses::summaryExplainsWhenEmpty()
{
    const QString text = lanrdp::lanAddressSummary(QStringList());
    QVERIFY2(!text.isEmpty(), "空列表也要给一句话");
    QVERIFY2(text.contains(QStringLiteral("网络")), qPrintable(text));
}

// 复制出去的内容要能直接粘给对方用：一行一个 ip:端口。
void TestLanAddresses::cardTextIsOneEndpointPerLine()
{
    const QStringList addrs = {QStringLiteral("192.168.1.104"), QStringLiteral("192.168.11.3")};
    QCOMPARE(lanrdp::connectionCardText(addrs, 9002),
             QStringLiteral("192.168.1.104:9002\n192.168.11.3:9002"));
    QVERIFY(lanrdp::connectionCardText(QStringList(), 9002).isEmpty());
}

// 邀请串是「照抄就能连上」的文本：三行分别对应【新连接…】里的三个框。
// 顺序固定 地址 / 端口 / 口令 —— 与对话框里从上到下的次序一致，抄的时候不会串行。
void TestLanAddresses::inviteTextCarriesTheThreeFields()
{
    const QStringList addrs = {QStringLiteral("192.168.1.104"), QStringLiteral("192.168.11.3")};
    QCOMPARE(lanrdp::inviteText(addrs, 9002, QStringLiteral("secret")),
             QStringLiteral("地址 192.168.1.104、192.168.11.3\n"
                            "端口 9002\n"
                            "口令 secret"));
}

// 没填口令时不能留一行空的「口令 」——那看起来像口令是空的，
// 而空口令在对端是**几乎必被拒**的（见 PasswordPolicy）。
void TestLanAddresses::inviteTextOmitsThePasswordLineWhenEmpty()
{
    QCOMPARE(lanrdp::inviteText({QStringLiteral("10.0.0.7")}, 9000, QString()),
             QStringLiteral("地址 10.0.0.7\n端口 9000"));
}

// 一个局域网地址都没有时（网线插着但没拿到 IP），不该产出一段「地址 」的残句：
// 空串让调用方能把按钮直接禁掉。
void TestLanAddresses::inviteTextIsEmptyWithoutAddresses()
{
    QVERIFY(lanrdp::inviteText(QStringList(), 9000, QStringLiteral("secret")).isEmpty());
}

namespace {

lanrdp::LocalSubnet subnet(const char *network, int prefixLength)
{
    return lanrdp::LocalSubnet{QHostAddress(QString::fromLatin1(network)), prefixLength};
}

} // namespace

// 同一个子网里的对端必须放行 —— 这是本功能唯一要保住的那条路。
void TestLanAddresses::peerInsideLocalSubnetIsAllowed()
{
    const QList<lanrdp::LocalSubnet> subnets = {subnet("192.168.1.0", 24)};
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("192.168.1.7")), subnets));
    // 本机自己的地址同样在子网内（对端与我们共用一台机器的情形）
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("192.168.1.104")), subnets));
}

// 别的网段（VPN / 热点 / 隔壁办公网）一律拒。
void TestLanAddresses::peerOutsideSubnetIsRejected()
{
    const QList<lanrdp::LocalSubnet> subnets = {subnet("192.168.1.0", 24)};
    QVERIFY(!lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("10.0.0.7")), subnets));
    // 前缀边界：192.168.2.x 与 192.168.1.0/24 只差在网络位
    QVERIFY(!lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("192.168.2.7")), subnets));
    // /16 与 /24 的区别要真的生效：172.16.0.0/16 收 172.16.255.7
    const QList<lanrdp::LocalSubnet> wide = {subnet("172.16.0.0", 16)};
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("172.16.255.7")), wide));
    QVERIFY(!lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("172.17.0.7")), wide));
}

// 回环一律放行：单机双开、本机自测（以及所有走 127.0.0.1 的 ctest）都指着它。
// 哪怕一块网卡都没有，它也必须通。
void TestLanAddresses::peerLoopbackIsAlwaysAllowed()
{
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress::LocalHost, {}));
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("127.0.0.5")), {}));
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("::1")), {}));
}

// 多网卡（有线 + 无线 + 虚拟机桥接）：任一子网命中即放行。
void TestLanAddresses::peerMatchesAnyOfSeveralSubnets()
{
    const QList<lanrdp::LocalSubnet> subnets = {
        subnet("192.168.1.0", 24),
        subnet("192.168.11.0", 24),
        subnet("172.23.96.0", 20),   // 典型的 vEthernet (WSL)
    };
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("192.168.11.3")), subnets));
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("172.23.100.9")), subnets));
    QVERIFY(!lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("192.168.12.3")), subnets));
}

// 双栈监听下对端常报成 `::ffff:a.b.c.d`；不归一化就会把同网段的机器全挡掉。
void TestLanAddresses::peerMappedIpv4StillMatchesSubnets()
{
    const QList<lanrdp::LocalSubnet> subnets = {subnet("192.168.1.0", 24)};
    const QHostAddress mapped(QStringLiteral("::ffff:192.168.1.7"));
    QVERIFY(lanrdp::isPeerAllowed(mapped, subnets));
    QVERIFY(!lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("::ffff:10.0.0.7")), subnets));
}

// IPv6：按前缀长度比，不是按字符串前缀比。
void TestLanAddresses::peerIpv6UsesThePrefix()
{
    const QList<lanrdp::LocalSubnet> subnets = {subnet("fe80::", 64)};
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("fe80::1234:5678:9abc:def0")),
                                  subnets));
    QVERIFY(!lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("2001:db8::1")), subnets));
    // 前缀只有 32 位时，后半段不同也该放行
    const QList<lanrdp::LocalSubnet> shortPrefix = {subnet("2001:db8::", 32)};
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("2001:db8:ffff::1")), shortPrefix));
    QVERIFY(!lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("2001:db9::1")), shortPrefix));
}

// 一块可用网卡都没有（网线插着但没拿到 IP）：只留回环那条路。
// 这**不是**「全拒」—— 全拒会把单机自测也一并打死。
void TestLanAddresses::peerWithoutAnySubnetAllowsOnlyLoopback()
{
    QVERIFY(lanrdp::isPeerAllowed(QHostAddress::LocalHost, {}));
    QVERIFY(!lanrdp::isPeerAllowed(QHostAddress(QStringLiteral("192.168.1.7")), {}));
}

QTEST_MAIN(TestLanAddresses)
#include "test_lanaddresses.moc"
