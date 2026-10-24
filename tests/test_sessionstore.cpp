#include "app/SessionStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

// 会话参数落盘：往返一致、口令不以明文出现、坏数据不炸、停用时不碰文件系统。
//
// ⚠️ 每条用例都用自己的临时目录。共用一个文件的话，"上一条留下的口令"会成为
// 下一条的输入，一旦失败看不出是谁写坏的。
class TestSessionStore : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsSecretThroughDpapi();
    void roundTripsHostSession();
    void roundTripsPeerSession();
    void keepsPasswordOutOfPlainText();
    void ignoresCorruptedSecret();
    void fallsBackOnInsaneValues();
    void disabledStoreTouchesNothing();
    void defaultPathIsUsable();
};

namespace {

QString iniPath(const QTemporaryDir &dir)
{
    return QDir(dir.path()).absoluteFilePath(QStringLiteral("session.ini"));
}

lanrdp::HostSession sampleHost()
{
    lanrdp::HostSession session;
    session.port       = 9111;
    session.password   = QStringLiteral("secret7");
    session.receiveDir = QStringLiteral("D:/tmp/received");
    session.useH264    = false;
    return session;
}

} // namespace

// DPAPI 往返。它是口令落盘的全部保护，坏掉的话写进 ini 的会是一串解不回来的
// 密文 —— 用户每次开程序仍要重输，而且没有任何报错。
void TestSessionStore::roundTripsSecretThroughDpapi()
{
    const QString plain  = QStringLiteral("secret7");
    const QString cipher = lanrdp::protectSecret(plain);

    QVERIFY2(!cipher.isEmpty(), "加密不应失败");
    QVERIFY2(cipher != plain, "密文不能等于明文");
    QCOMPARE(lanrdp::unprotectSecret(cipher), plain);

    // 空口令不该被包装成"记住了一个空口令"。
    QVERIFY(lanrdp::protectSecret(QString()).isEmpty());
    QVERIFY(lanrdp::unprotectSecret(QString()).isEmpty());
}

void TestSessionStore::roundTripsHostSession()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    lanrdp::SessionStore store(iniPath(dir));

    const lanrdp::HostSession in = sampleHost();
    QVERIFY2(store.saveHost(in), "保存应当成功");

    const lanrdp::HostSession out = store.loadHost();
    QCOMPARE(out.port, in.port);
    QCOMPARE(out.password, in.password);
    QCOMPARE(out.receiveDir, in.receiveDir);
    QCOMPARE(out.useH264, in.useH264);
}

void TestSessionStore::roundTripsPeerSession()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    lanrdp::SessionStore store(iniPath(dir));

    lanrdp::PeerSession in;
    in.host     = QStringLiteral("192.168.1.42");
    in.port     = 9222;
    in.password = QStringLiteral("secret7");
    QVERIFY2(store.savePeer(in), "保存应当成功");

    const lanrdp::PeerSession out = store.loadPeer();
    QCOMPARE(out.host, in.host);
    QCOMPARE(out.port, in.port);
    QCOMPARE(out.password, in.password);
}

// ini 是纯文本，用户/备份工具/同步盘都能读到。口令一旦以明文躺在里面，
// 「至少 6 位」就退化成了摆设 —— 所以这条断言是这个功能的安全底线。
void TestSessionStore::keepsPasswordOutOfPlainText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    lanrdp::SessionStore store(iniPath(dir));
    QVERIFY(store.saveHost(sampleHost()));

    QFile file(store.filePath());
    QVERIFY2(file.open(QIODevice::ReadOnly), "应当能读到 ini");
    const QByteArray raw = file.readAll();
    file.close();

    const QByteArray plain = QByteArrayLiteral("secret7");
    QVERIFY2(!raw.contains(plain), "ini 里出现了明文口令");

    // 也不能是 UTF-16 形式（QString 直接 toLocal8Bit 之外的写法同样会被搜到）。
    QByteArray utf16;
    for (const char c : plain) {
        utf16.append(c);
        utf16.append('\0');
    }
    QVERIFY2(!raw.contains(utf16), "ini 里出现了 UTF-16 形式的明文口令");
}

// 换用户 / 换机器 / 手工改坏 ini —— 解不开必须退化成"没存过"，而不是把乱码
// 当成口令发出去（那会变成一次莫名其妙的握手失败）。
void TestSessionStore::ignoresCorruptedSecret()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    lanrdp::SessionStore store(iniPath(dir));
    QVERIFY(store.saveHost(sampleHost()));

    {
        QSettings ini(store.filePath(), QSettings::IniFormat);
        ini.setValue(QStringLiteral("host/password"),
                     QStringLiteral("bm90LWEtcmVhbC1jaXBoZXI="));   // "not-a-real-cipher"
        ini.sync();
    }

    const lanrdp::HostSession out = store.loadHost();
    QVERIFY2(out.password.isEmpty(), "解不开的口令应当当作没存过");
    QCOMPARE(out.port, quint16(9111));   // 其它字段不受牵连
    QCOMPARE(out.receiveDir, QStringLiteral("D:/tmp/received"));
}

// ini 是人能编辑的文件，读到 0 / 70000 / 字母都不奇怪。非法值必须退回默认，
// 不能带进 listen()。
void TestSessionStore::fallsBackOnInsaneValues()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    lanrdp::SessionStore store(iniPath(dir));
    QVERIFY(store.saveHost(sampleHost()));

    {
        QSettings ini(store.filePath(), QSettings::IniFormat);
        ini.setValue(QStringLiteral("host/port"), 0);
        ini.setValue(QStringLiteral("peer/port"), 70000);
        ini.setValue(QStringLiteral("peer/host"), QStringLiteral("   "));
        ini.sync();
    }

    QCOMPARE(store.loadHost().port, quint16(9000));   // qint16 的下界以下
    QCOMPARE(store.loadPeer().port, quint16(9000));   // 越界
    QCOMPARE(store.loadPeer().host, QStringLiteral("127.0.0.1"));   // 全空白
}

// 空路径 = 停用。测试与自动化默认走这条，它们绝不能读写真实配置。
void TestSessionStore::disabledStoreTouchesNothing()
{
    lanrdp::SessionStore store;
    QVERIFY(!store.isEnabled());
    QVERIFY(store.filePath().isEmpty());

    QVERIFY2(!store.saveHost(sampleHost()), "停用状态不该报告保存成功");
    QVERIFY(store.loadHost().password.isEmpty());
    QVERIFY(store.loadPeer().password.isEmpty());
}

void TestSessionStore::defaultPathIsUsable()
{
    const QString path = lanrdp::defaultSessionFilePath();
    QVERIFY2(!path.isEmpty(), "默认路径不能为空");
    QVERIFY2(path.endsWith(QStringLiteral("session.ini")), qPrintable(path));
    QVERIFY2(path.contains(QStringLiteral("lanrdp")), qPrintable(path));
    // 必须是绝对路径：相对路径会跟着进程的工作目录跑（Explorer 启动时不保证是 exe 目录）。
    QVERIFY2(QFileInfo(path).isAbsolute(), qPrintable(path));
}

QTEST_GUILESS_MAIN(TestSessionStore)
#include "test_sessionstore.moc"
