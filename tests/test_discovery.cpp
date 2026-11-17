#include <QtTest/QtTest>

#include <QSignalSpy>
#include <QString>
#include <QUdpSocket>

#include "net/Discovery.h"

// 局域网自动发现：报文编解码 + 两个实例互相看得见 / 看不见自己 / 过期下线。
//
// 用例里两个 Discovery 都绑**系统分配的端口**（port=0），广播目标手动指向
// 127.0.0.1 —— 不往真实局域网里发一个包（测试机上可能正跑着别的实例）。
class TestDiscovery : public QObject
{
    Q_OBJECT

private slots:
    // ---- 报文 ----
    void beaconRoundTrip();
    void beaconRejectsMalformed();
    // ---- 行为 ----
    void peersSeeEachOther();
    void ownBeaconIsIgnored();
    void silentPeerExpires();
    // 同一台机器两条网线时，两条路都要列出来（点哪条连哪条）
    void sameMachineKeepsEveryAddress();
    void staleAddressDropsWhilePeerStays();
    void repeatedBeaconsDoNotReshuffle();
};

void TestDiscovery::beaconRoundTrip()
{
    lanrdp::Beacon beacon;
    beacon.instanceId = 0xDEADBEEF;
    beacon.port       = 9000;
    beacon.name       = QStringLiteral("DESKTOP-测试");

    const QByteArray datagram = lanrdp::encodeBeacon(beacon);
    QVERIFY(!datagram.isEmpty());
    QCOMPARE(datagram.left(4), QByteArrayLiteral("LRD1"));

    const auto decoded = lanrdp::decodeBeacon(datagram);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->instanceId, beacon.instanceId);
    QCOMPARE(decoded->port, beacon.port);
    QCOMPARE(decoded->name, beacon.name);
}

void TestDiscovery::beaconRejectsMalformed()
{
    lanrdp::Beacon good;
    good.instanceId = 7;
    good.port       = 9000;
    good.name       = QStringLiteral("A");
    const QByteArray datagram = lanrdp::encodeBeacon(good);
    QVERIFY(!datagram.isEmpty());

    // 端口 0 编不出来（连不上的条目不该出现在列表里）
    lanrdp::Beacon noPort = good;
    noPort.port = 0;
    QVERIFY(lanrdp::encodeBeacon(noPort).isEmpty());

    // 实例 id 0 同样不是一个合法身份
    lanrdp::Beacon noId = good;
    noId.instanceId = 0;
    QVERIFY(lanrdp::encodeBeacon(noId).isEmpty());

    // 魔数不对
    QByteArray wrongMagic = datagram;
    wrongMagic[0] = 'X';
    QVERIFY(!lanrdp::decodeBeacon(wrongMagic).has_value());

    // 截断
    QVERIFY(!lanrdp::decodeBeacon(datagram.left(datagram.size() - 1)).has_value());

    // 多出字节（布局对不上）
    QByteArray trailing = datagram;
    trailing.append('x');
    QVERIFY(!lanrdp::decodeBeacon(trailing).has_value());

    // 空 / 超长
    QVERIFY(!lanrdp::decodeBeacon({}).has_value());
    QVERIFY(!lanrdp::decodeBeacon(QByteArray(5000, 'x')).has_value());

    // 名字超长编不出来（截断由 Discovery 自己做，编解码层直接拒绝）
    lanrdp::Beacon longName = good;
    longName.name = QString(lanrdp::kMaxBeaconNameBytes + 1, QLatin1Char('n'));
    QVERIFY(lanrdp::encodeBeacon(longName).isEmpty());
}

// 一个在监听、一个在找：找的那头应当看到「名字 + 地址 + TCP 端口」。
void TestDiscovery::peersSeeEachOther()
{
    lanrdp::Discovery host;
    lanrdp::Discovery viewer;
    QVERIFY(host.start(0));
    QVERIFY(viewer.start(0));
    QVERIFY(host.listenPort() != 0);
    QVERIFY(viewer.listenPort() != 0);

    QSignalSpy changed(&viewer, &lanrdp::Discovery::peersChanged);

    // 只让 host 广播，goal 只收
    host.setBroadcastTargets({{QHostAddress::LocalHost, viewer.listenPort()}});
    host.setAdvertisePort(9000);

    QTRY_COMPARE_WITH_TIMEOUT(viewer.peers().size(), 1, 3000);
    const auto peer = viewer.peers().first();
    QCOMPARE(peer.port, quint16(9000));
    QCOMPARE(peer.instanceId, host.instanceId());
    QCOMPARE(peer.address, QHostAddress(QHostAddress::LocalHost));
    QVERIFY(!peer.name.isEmpty());
    QVERIFY(changed.count() >= 1);

    // 端口改了要能看见（同一台机器换了监听端口）—— 不然列表会指着旧端口
    host.setAdvertisePort(9100);
    QTRY_COMPARE_WITH_TIMEOUT(viewer.peers().first().port, quint16(9100), 3000);
}

// 广播会回灌到发包方自己（同一台机器绑同一个端口时尤其明显）：
// 自己的实例 id 必须认出来并丢掉，否则列表里会出现「我自己」。
void TestDiscovery::ownBeaconIsIgnored()
{
    lanrdp::Discovery self;
    QVERIFY(self.start(0));
    self.setBroadcastTargets({{QHostAddress::LocalHost, self.listenPort()}});
    self.setAdvertisePort(9000);

    QSignalSpy changed(&self, &lanrdp::Discovery::peersChanged);
    self.sendBeaconNow();

    // 给足事件循环：真收到了自己的包就会有 peersChanged
    QTest::qWait(150);
    QVERIFY2(self.peers().isEmpty(), "不该把自己当成一台可连的机器");
    QCOMPARE(changed.count(), 0);
}

// 对方不再广播（关掉监听 / 进程退出）后，列表必须自己把条目拿走：
// 留着一条点了连不上的记录，比没有自动发现更糟。
void TestDiscovery::silentPeerExpires()
{
    lanrdp::Discovery host;
    lanrdp::Discovery viewer;
    QVERIFY(host.start(0));
    QVERIFY(viewer.start(0));

    viewer.setExpiryMs(60);
    viewer.setIntervalMs(20);   // 过期清理跟着 tick 走，压短便于观察

    host.setBroadcastTargets({{QHostAddress::LocalHost, viewer.listenPort()}});
    host.setIntervalMs(20);
    host.setAdvertisePort(9000);

    QTRY_COMPARE_WITH_TIMEOUT(viewer.peers().size(), 1, 3000);

    // 不再广播（等价于对方停止监听）
    host.setAdvertisePort(0);
    QTRY_VERIFY_WITH_TIMEOUT(viewer.peers().isEmpty(), 3000);
}

// 一台机器同时接 Wi-Fi 和网线时，同一实例的广播会从**两个源地址**各来一份。
// 两条路都要留在列表里（哪条通只有用户知道），但整台机器仍然只算一台。
//
// 这里用 127.0.0.1 / 127.0.0.2 冒充两块网卡：127.0.0.0/8 整段都是回环地址，
// 绑上去发出去的包，接收方看到的源地址就是那个 IP。
void TestDiscovery::sameMachineKeepsEveryAddress()
{
    lanrdp::Discovery viewer;
    QVERIFY(viewer.start(0));
    QVERIFY(viewer.listenPort() != 0);

    lanrdp::Beacon beacon;
    beacon.instanceId = 20261117;
    beacon.port       = 9000;
    beacon.name       = QStringLiteral("双网卡机");
    const QByteArray datagram = lanrdp::encodeBeacon(beacon);
    QVERIFY(!datagram.isEmpty());

    QUdpSocket wired;
    QUdpSocket wireless;
    // 绑定失败就 skip：某些环境（容器里）没有整段回环，这条用例的前提不成立。
    if (!wired.bind(QHostAddress(QStringLiteral("127.0.0.1")), 0)
        || !wireless.bind(QHostAddress(QStringLiteral("127.0.0.2")), 0)) {
        QSKIP("本机不支持 127.0.0.2 回环地址，无法模拟双网卡");
    }

    wired.writeDatagram(datagram, QHostAddress::LocalHost, viewer.listenPort());
    QTRY_COMPARE_WITH_TIMEOUT(viewer.peers().size(), 1, 3000);
    QCOMPARE(viewer.peers().first().addresses.size(), 1);

    wireless.writeDatagram(datagram, QHostAddress::LocalHost, viewer.listenPort());
    QTRY_COMPARE_WITH_TIMEOUT(viewer.peers().first().addresses.size(), 2, 3000);

    // 还是**一台**机器：多出来的只是它的另一条路。
    QCOMPARE(viewer.peers().size(), 1);
    const auto peer = viewer.peers().first();
    QCOMPARE(peer.instanceId, beacon.instanceId);
    QCOMPARE(peer.port, quint16(9000));
    // 首选地址取数值最小的那条（排序稳定，不随包到得早晚变）
    QCOMPARE(peer.address, QHostAddress(QHostAddress::LocalHost));
}

// 拔掉一条网线：消失的只该是那一行，不能把整台机器从列表里删掉。
void TestDiscovery::staleAddressDropsWhilePeerStays()
{
    lanrdp::Discovery viewer;
    QVERIFY(viewer.start(0));
    viewer.setExpiryMs(300);
    viewer.setIntervalMs(20);   // 过期清理跟着 tick 走，压短便于观察

    lanrdp::Beacon beacon;
    beacon.instanceId = 20261118;
    beacon.port       = 9000;
    beacon.name       = QStringLiteral("拔网线机");
    const QByteArray datagram = lanrdp::encodeBeacon(beacon);

    QUdpSocket wired;
    QUdpSocket wireless;
    if (!wired.bind(QHostAddress(QStringLiteral("127.0.0.1")), 0)
        || !wireless.bind(QHostAddress(QStringLiteral("127.0.0.2")), 0)) {
        QSKIP("本机不支持 127.0.0.2 回环地址，无法模拟双网卡");
    }

    wired.writeDatagram(datagram, QHostAddress::LocalHost, viewer.listenPort());
    wireless.writeDatagram(datagram, QHostAddress::LocalHost, viewer.listenPort());
    // 先等两条路都到齐（列表为空时不能直接取 first）
    QTRY_VERIFY_WITH_TIMEOUT(
        !viewer.peers().isEmpty() && viewer.peers().first().addresses.size() == 2, 3000);

    // 之后只有 127.0.0.1 那条继续心跳，另一条「拔了」。
    for (int i = 0; i < 20; ++i) {
        wired.writeDatagram(datagram, QHostAddress::LocalHost, viewer.listenPort());
        QTest::qWait(30);
    }

    const auto peers = viewer.peers();
    QCOMPARE(peers.size(), 1);                          // 机器还在
    QCOMPARE(peers.first().addresses.size(), 1);        // 只是少了一条路
    QCOMPARE(peers.first().addresses.first().address,
             QHostAddress(QHostAddress::LocalHost));
}

// 同一批机器反复广播时，列表**不该反复重建**（重建会丢选中项与滚动位置，
// 肉眼就是一直闪）。这里用一个较短的间隔跑几轮，peersChanged 必须停在 1 次。
void TestDiscovery::repeatedBeaconsDoNotReshuffle()
{
    lanrdp::Discovery host;
    lanrdp::Discovery viewer;
    QVERIFY(host.start(0));
    QVERIFY(viewer.start(0));

    QSignalSpy changed(&viewer, &lanrdp::Discovery::peersChanged);

    host.setBroadcastTargets({{QHostAddress::LocalHost, viewer.listenPort()}});
    host.setIntervalMs(20);
    host.setAdvertisePort(9000);

    QTRY_COMPARE_WITH_TIMEOUT(viewer.peers().size(), 1, 3000);
    QCOMPARE(changed.count(), 1);   // 第一次看到它：应该只报一次

    // 再听十来轮相同的报文：内容没变就不该再发 peersChanged
    QTest::qWait(300);
    QCOMPARE(viewer.peers().size(), 1);
    QVERIFY2(changed.count() == 1,
             qPrintable(QStringLiteral("相同的广播不该反复刷新列表（peersChanged 次数 %1）")
                            .arg(changed.count())));
}

QTEST_GUILESS_MAIN(TestDiscovery)
#include "test_discovery.moc"
