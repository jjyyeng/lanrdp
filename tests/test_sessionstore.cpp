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
    // 界面外观（ui/theme）：与语言同一个 ini、互不覆盖
    void roundTripsUiTheme();
    // 三项行为偏好（app/*）的三态：没存过 / on / off 必须分得开
    void roundTripsBehaviorSwitches();
    void keepsPasswordOutOfPlainText();
    void ignoresCorruptedSecret();
    void fallsBackOnInsaneValues();
    void disabledStoreTouchesNothing();
    void defaultPathIsUsable();
    // 保存失败必须**给得出原因**：调用方要把它写进日志，否则用户只知道"没记住"
    void failedSaveReportsReason();
    // 用户主动清空口令 ⇒ 旧密文必须真的被清掉（不能因为"怕覆盖"就一直留着）
    void clearingPasswordRemovesStoredCipher();
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
    session.bitrateBps = 12 * 1000 * 1000;
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
    QCOMPARE(out.bitrateBps, in.bitrateBps);
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

// 外观偏好与语言同住一个 ini：写其中一项不能把另一项冲掉
//（QSettings 只改自己那个键，但这条约定值得钉住 —— 冲掉的表现是「换主题之后
//  界面语言回到中文」，用户根本不会把两件事联系起来）。
void TestSessionStore::roundTripsUiTheme()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    lanrdp::SessionStore store(iniPath(dir));

    // 没存过 ⇒ 空串。**不返回默认键**：「没选过」与「明确选了深色」是两件事。
    QVERIFY2(store.loadUiTheme().isEmpty(), "没存过应当是空串，由调用方决定默认值");

    QVERIFY(store.saveUiTheme(QStringLiteral("light")));
    QCOMPARE(store.loadUiTheme(), QStringLiteral("light"));

    QVERIFY(store.saveUiLanguage(QStringLiteral("en")));
    QCOMPARE(store.loadUiTheme(), QStringLiteral("light"));      // 语言没把主题冲掉
    QCOMPARE(store.loadUiLanguage(), QStringLiteral("en"));

    QVERIFY(store.saveUiTheme(QStringLiteral("dark")));
    QCOMPARE(store.loadUiTheme(), QStringLiteral("dark"));       // 能改回去
    QCOMPARE(store.loadUiLanguage(), QStringLiteral("en"));      // 主题没把语言冲掉
}

// 三项行为偏好与语言/主题同住一个 ini。要守住三件事：
//   ① 「没存过」必须是 nullopt —— 否则首启套不上各自的默认值，用户也永远回不到默认；
//   ② 存 false 之后必须读回 false（不能因为 false 就等于「没存过」）；
//   ③ 写这三项不能把语言/主题或口令参数冲掉。
void TestSessionStore::roundTripsBehaviorSwitches()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    lanrdp::SessionStore store(iniPath(dir));

    const QString keys[] = {lanrdp::appAutoReconnectKey(), lanrdp::appReceiveAudioKey(),
                            lanrdp::appTrayResidentKey(), lanrdp::appSingleInstanceKey()};
    for (const QString &key : keys) {
        QVERIFY2(!key.isEmpty(), "键名不能为空");
        QVERIFY2(!store.loadAppSwitch(key).has_value(),
                 qPrintable(QStringLiteral("%1 没存过时必须返回 nullopt").arg(key)));
    }

    // 三项分开存：勾一项不该顺手把另外两项也写成 true
    QVERIFY(store.saveAppSwitch(lanrdp::appTrayResidentKey(), true));
    QVERIFY2(store.loadAppSwitch(lanrdp::appTrayResidentKey()).value_or(false),
             "存了 true 应当读回 true");
    QVERIFY2(!store.loadAppSwitch(lanrdp::appSingleInstanceKey()).has_value(),
             "只勾了托盘，单实例应当仍是「没存过」");
    QVERIFY2(!store.loadAppSwitch(lanrdp::appAutoReconnectKey()).has_value(),
             "只勾了托盘，自动重连应当仍是「没存过」");
    QVERIFY2(!store.loadAppSwitch(lanrdp::appReceiveAudioKey()).has_value(),
             "只勾了托盘，声音应当仍是「没存过」");

    // 关掉必须能读回「明确关掉了」
    QVERIFY(store.saveAppSwitch(lanrdp::appAutoReconnectKey(), false));
    const auto reconnect = store.loadAppSwitch(lanrdp::appAutoReconnectKey());
    QVERIFY2(reconnect.has_value(), "存了 false 也算存过 —— 不能退化成「没存过」");
    QVERIFY(!*reconnect);

    // 写别的键不该互相冲掉 —— 冲掉的表现是「换了主题，单实例又变回默认」
    QVERIFY(store.saveUiTheme(QStringLiteral("light")));
    QVERIFY(store.saveAppSwitch(lanrdp::appSingleInstanceKey(), true));
    QCOMPARE(store.loadUiTheme(), QStringLiteral("light"));
    QVERIFY(store.loadAppSwitch(lanrdp::appTrayResidentKey()).value_or(false));
    QVERIFY(store.loadAppSwitch(lanrdp::appSingleInstanceKey()).value_or(false));

    // 停用时一律 nullopt / 写失败（测试与自动化不会碰真实配置）
    lanrdp::SessionStore disabled;
    QVERIFY(!disabled.loadAppSwitch(lanrdp::appTrayResidentKey()).has_value());
    QVERIFY(!disabled.saveAppSwitch(lanrdp::appTrayResidentKey(), true));
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
        // 码率：0 会被编码器的下限静默抬成 100 kbps，10 位数字则是胡说 —— 两者都退回默认。
        ini.setValue(QStringLiteral("host/bitrateBps"), 0);
        ini.sync();
    }

    QCOMPARE(store.loadHost().port, quint16(9000));   // qint16 的下界以下
    QCOMPARE(store.loadPeer().port, quint16(9000));   // 越界
    QCOMPARE(store.loadPeer().host, QStringLiteral("127.0.0.1"));   // 全空白
    QCOMPARE(store.loadHost().bitrateBps, 8 * 1000 * 1000);

    {
        QSettings ini(store.filePath(), QSettings::IniFormat);
        ini.setValue(QStringLiteral("host/bitrateBps"), 100 * 1000 * 1000 + 1);
        ini.sync();
    }
    QCOMPARE(store.loadHost().bitrateBps, 8 * 1000 * 1000);
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

// 保存失败时必须给得出「为什么」，否则调用方只能记一句"没保存"，用户拿着日志也
// 不知道该修什么（目录只读？磁盘满？口令加密失败？三种处理完全不同）。
//
// 用「停用」这条必定失败的路径来钉出参约定。⚠️ 真正危险的另一条 —— 口令加密失败
// 却被 `QSettings::status()` 判成成功 —— 在正常环境里制造不出来（要 DPAPI 失败），
// 只能靠实现里的分支保证：`writeSecret()` 加密失败时**不写那个键**并返回原因。
void TestSessionStore::failedSaveReportsReason()
{
    lanrdp::SessionStore store;   // 空路径 = 停用

    QString error;
    QVERIFY2(!store.saveHost(sampleHost(), &error), "停用状态不该报告保存成功");
    QVERIFY2(!error.isEmpty(), "失败必须填出原因（调用方要写进日志）");

    lanrdp::PeerSession peer;
    peer.host     = QStringLiteral("10.0.0.9");
    peer.port     = 9100;
    peer.password = QStringLiteral("secret7");
    error.clear();
    QVERIFY2(!store.savePeer(peer, &error), "停用状态不该报告保存成功");
    QVERIFY2(!error.isEmpty(), "失败必须填出原因（调用方要写进日志）");

    // 出参是可选的：不传也不能崩，返回值仍是同一个事实。
    QVERIFY(!store.saveHost(sampleHost()));
}

// 用户主动清空口令 ⇒ 旧密文必须真的被清掉。
//
// 这条和「加密失败不许用空串覆盖旧密文」是一对：两者都发生在"新口令为空"附近，
// 但语义相反。若为了保住旧密文而无条件跳过写入，用户清空口令后下次启动会看到
// 它又回来了 —— 那是另一种"我说了不算"。
void TestSessionStore::clearingPasswordRemovesStoredCipher()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = iniPath(dir);

    lanrdp::SessionStore store(path);
    lanrdp::HostSession host = sampleHost();
    QString error;
    QVERIFY2(store.saveHost(host, &error), qPrintable(error));
    QVERIFY2(error.isEmpty(), "成功时不该留下失败原因");

    {
        QSettings ini(path, QSettings::IniFormat);
        ini.sync();
        QVERIFY2(!ini.value(QStringLiteral("host/password")).toString().isEmpty(),
                 "口令非空时 ini 里应当有密文");
    }

    host.password.clear();
    QVERIFY2(store.saveHost(host, &error), qPrintable(error));

    QSettings after(path, QSettings::IniFormat);
    after.sync();
    QVERIFY2(after.value(QStringLiteral("host/password")).toString().isEmpty(),
             "用户清空口令后，旧密文必须被清掉");
    // 其余字段不受影响 —— 清口令不该顺手把端口/目录一起清掉。
    QCOMPARE(store.loadHost().port, quint16(9111));
    QCOMPARE(store.loadHost().receiveDir, QStringLiteral("D:/tmp/received"));
}

QTEST_GUILESS_MAIN(TestSessionStore)
#include "test_sessionstore.moc"
