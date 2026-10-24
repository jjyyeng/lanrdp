#include <QtTest/QtTest>

#include <QStringList>

#include "app/CliArgs.h"

// 命令行参数解析。
//
// 从前这约 200 行分支全挤在 `main()` 里，而 `main.cpp` 带着 `main()` 没法链进
// 任何测试目标 ⇒ 一条断言都没有。静默吞参数的老毛病就是这么长期存在的：
// 未知参数被忽略、`--host 9000` 漏口令时端口被当成口令、`toInt()` 失败静默变 0。
class TestCliArgs : public QObject
{
    Q_OBJECT

private slots:
    // ---- 正常路径 ----
    void defaultsToGuiMode();
    void parsesHostMode();
    void parsesConnectModeWithPort();
    void connectWithoutPortKeepsDefaultPort();
    void expectFrameTakesOptionalNumber();
    void parsesCursorPosAndInjectLocal();
    void parsesLogFileAndRecvDir();

    // ---- 错误路径：一律 ok=false 且带可读原因 ----
    void rejectsUnknownArgument();
    void rejectsHostMissingPassword();
    void rejectsHostWithBadPort();
    void rejectsConnectMissingPassword();
    void rejectsMissingValues();
    void rejectsBadNumbers();
    void rejectsHostAndConnectTogether();
    void rejectsMultipleActions();
    void rejectsActionWithoutConnect();
    void rejectsWeakHostPassword();

    // --bitrate 非法**不算**参数错误（沿用默认），与上面那批区分开
    void invalidBitrateFallsBackToDefault();
};

namespace {

// 解析一串参数（自动补上 argv[0]，与 QCoreApplication::arguments() 同口径）
lanrdp::CliParseResult parse(const QStringList &args)
{
    QStringList withArgv0;
    withArgv0 << QStringLiteral("lanrdp") << args;
    return lanrdp::parseCliArgs(withArgv0);
}

} // namespace

void TestCliArgs::defaultsToGuiMode()
{
    const lanrdp::CliParseResult r = parse({});
    QVERIFY2(r.ok, qPrintable(r.error));

    const lanrdp::CliOptions &o = r.options;
    QVERIFY2(!o.hostMode && !o.connectMode, "无参数应当是普通 GUI 模式");
    QVERIFY(!o.quiet);
    QVERIFY(!o.quitOnResult);
    QVERIFY2(!o.cursorPosOnly && !o.injectLocal, "无参数不该进入自检模式");
    QVERIFY(!o.sendInput);
    QVERIFY(!o.useRawFrames);

    QCOMPARE(o.hostPort, quint16(9000));
    QCOMPARE(o.peerPort, quint16(9000));
    QCOMPARE(o.expectFrameMs, 0);
    QCOMPARE(o.measureSeconds, 0);
    QCOMPARE(o.sendWaitMs, 400);
    QCOMPARE(o.holdTokenMs, 0);
    QCOMPARE(o.tokenWaitMs, 2000);
    QCOMPARE(o.bitrateBps, 8 * 1000 * 1000);

    QVERIFY(o.password.isEmpty());
    QVERIFY(o.peerHost.isEmpty());
    QVERIFY(o.recvDir.isEmpty());
    QVERIFY(o.sendFilePath.isEmpty());
    QVERIFY(o.logFilePath.isEmpty());
}

void TestCliArgs::parsesHostMode()
{
    const lanrdp::CliParseResult r =
        parse({ QStringLiteral("--host"), QStringLiteral("9100"), QStringLiteral("secret"),
                QStringLiteral("--raw"), QStringLiteral("--bitrate"), QStringLiteral("4000000"),
                QStringLiteral("--recv-dir"), QStringLiteral("D:/recv"),
                QStringLiteral("--quiet") });
    QVERIFY2(r.ok, qPrintable(r.error));

    const lanrdp::CliOptions &o = r.options;
    QVERIFY(o.hostMode);
    QVERIFY(!o.connectMode);
    QCOMPARE(o.hostPort, quint16(9100));
    QCOMPARE(o.password, QStringLiteral("secret"));
    QVERIFY2(o.useRawFrames, "--raw 应当强制裸帧");
    QCOMPARE(o.bitrateBps, 4000000);
    QCOMPARE(o.recvDir, QStringLiteral("D:/recv"));
    QVERIFY(o.quiet);
}

void TestCliArgs::parsesConnectModeWithPort()
{
    const lanrdp::CliParseResult r =
        parse({ QStringLiteral("--connect"), QStringLiteral("192.168.1.5:9200"),
                QStringLiteral("pw"), QStringLiteral("--quit-on-result") });
    QVERIFY2(r.ok, qPrintable(r.error));

    const lanrdp::CliOptions &o = r.options;
    QVERIFY(o.connectMode);
    QVERIFY(!o.hostMode);
    QCOMPARE(o.peerHost, QStringLiteral("192.168.1.5"));
    QCOMPARE(o.peerPort, quint16(9200));
    QCOMPARE(o.password, QStringLiteral("pw"));
    QVERIFY(o.quitOnResult);
}

// `--connect <host> <pw>`（不带 `:port`）是允许的 —— 端口沿用默认，不是错误。
void TestCliArgs::connectWithoutPortKeepsDefaultPort()
{
    const lanrdp::CliParseResult r = parse({ QStringLiteral("--connect"),
                                             QStringLiteral("myhost"),
                                             QStringLiteral("pw") });
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.options.peerHost, QStringLiteral("myhost"));
    QCOMPARE(r.options.peerPort, quint16(9000));
}

// `--expect-frame` 的取值是可选的：跟着数字就吃掉，不跟就留默认 15000。
void TestCliArgs::expectFrameTakesOptionalNumber()
{
    // 动作类参数必须配 --connect，所以这几条都带上最小的连接参数。
    const lanrdp::CliParseResult withNumber =
        parse({ QStringLiteral("--connect"), QStringLiteral("1.2.3.4:9000"),
                QStringLiteral("pw"), QStringLiteral("--expect-frame"),
                QStringLiteral("3000") });
    QVERIFY2(withNumber.ok, qPrintable(withNumber.error));
    QCOMPARE(withNumber.options.expectFrameMs, 3000);

    const lanrdp::CliParseResult bare =
        parse({ QStringLiteral("--connect"), QStringLiteral("1.2.3.4:9000"),
                QStringLiteral("pw"), QStringLiteral("--expect-frame"),
                QStringLiteral("--quiet") });
    QVERIFY2(bare.ok, qPrintable(bare.error));
    QCOMPARE(bare.options.expectFrameMs, 15000);
    QVERIFY2(bare.options.quiet, "选项没被当成取值吃掉");
}

void TestCliArgs::parsesCursorPosAndInjectLocal()
{
    const lanrdp::CliParseResult cursor = parse({ QStringLiteral("--cursor-pos") });
    QVERIFY2(cursor.ok, qPrintable(cursor.error));
    QVERIFY(cursor.options.cursorPosOnly);
    QVERIFY(!cursor.options.injectLocal);

    const lanrdp::CliParseResult inject =
        parse({ QStringLiteral("--inject-local"), QStringLiteral("100"), QStringLiteral("200") });
    QVERIFY2(inject.ok, qPrintable(inject.error));
    QVERIFY(inject.options.injectLocal);
    QCOMPARE(inject.options.localNx, quint16(100));
    QCOMPARE(inject.options.localNy, quint16(200));
}

void TestCliArgs::parsesLogFileAndRecvDir()
{
    const lanrdp::CliParseResult r = parse({ QStringLiteral("--log-file"),
                                             QStringLiteral("D:/x/lanrdp.log") });
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.options.logFilePath, QStringLiteral("D:/x/lanrdp.log"));
}

void TestCliArgs::rejectsUnknownArgument()
{
    const lanrdp::CliParseResult r = parse({ QStringLiteral("--silnet") });
    QVERIFY2(!r.ok, "拼错的参数必须报错，而不是被静默忽略");
    QVERIFY2(r.error.contains(QStringLiteral("未知参数")), qPrintable(r.error));
}

// 从前 `--host 9000`（漏口令）会让端口被当成口令用掉，然后直接进 GUI。
void TestCliArgs::rejectsHostMissingPassword()
{
    const lanrdp::CliParseResult r = parse({ QStringLiteral("--host"), QStringLiteral("9000") });
    QVERIFY(!r.ok);
    QVERIFY2(r.error.contains(QStringLiteral("--host")), qPrintable(r.error));
}

void TestCliArgs::rejectsHostWithBadPort()
{
    const lanrdp::CliParseResult r = parse({ QStringLiteral("--host"), QStringLiteral("abc"),
                                             QStringLiteral("pw") });
    QVERIFY(!r.ok);
    QVERIFY2(r.error.contains(QStringLiteral("端口非法")), qPrintable(r.error));
}

void TestCliArgs::rejectsConnectMissingPassword()
{
    const lanrdp::CliParseResult r = parse({ QStringLiteral("--connect"),
                                             QStringLiteral("1.2.3.4:9000") });
    QVERIFY(!r.ok);
    QVERIFY2(r.error.contains(QStringLiteral("--connect")), qPrintable(r.error));
}

void TestCliArgs::rejectsMissingValues()
{
    const QStringList options = { QStringLiteral("--recv-dir"), QStringLiteral("--send-file"),
                                  QStringLiteral("--log-file"), QStringLiteral("--bitrate") };
    for (const QString &opt : options) {
        const lanrdp::CliParseResult r = parse({ opt });
        QVERIFY2(!r.ok, qPrintable(QStringLiteral("%1 缺取值必须报错").arg(opt)));
        QVERIFY2(r.error.contains(QStringLiteral("缺少取值")), qPrintable(r.error));
    }
}

void TestCliArgs::rejectsBadNumbers()
{
    // 动作类参数要求配 --connect（parse 尾部有校验），先拼上最小连接参数，
    // 免得「缺 --connect」把这里真正要测的「数值非法」盖过去。
    const auto act = [](const QStringList &tail) {
        QStringList args{ QStringLiteral("--connect"), QStringLiteral("1.2.3.4:9000"),
                          QStringLiteral("pw") };
        args << tail;
        return parse(args);
    };

    // 0 / 负数 / 非数字都要报错 —— 从前 toInt() 失败会静默变 0
    QVERIFY(!act({ QStringLiteral("--measure-seconds"), QStringLiteral("0") }).ok);
    QVERIFY(!act({ QStringLiteral("--measure-seconds"), QStringLiteral("abc") }).ok);
    QVERIFY(!parse({ QStringLiteral("--token-wait"), QStringLiteral("0") }).ok);
    QVERIFY(!act({ QStringLiteral("--hold-token"), QStringLiteral("0") }).ok);
    QVERIFY(!parse({ QStringLiteral("--send-wait"), QStringLiteral("-1") }).ok);
    QVERIFY(!act({ QStringLiteral("--expect-frame"), QStringLiteral("abc") }).ok);
    QVERIFY(!act({ QStringLiteral("--expect-frame"), QStringLiteral("0") }).ok);
    QVERIFY(!act({ QStringLiteral("--send-input"), QStringLiteral("x"),
                   QStringLiteral("1") }).ok);
    QVERIFY(!parse({ QStringLiteral("--inject-local"), QStringLiteral("1"),
                     QStringLiteral("70000") }).ok);   // 超 quint16
}

void TestCliArgs::rejectsHostAndConnectTogether()
{
    const lanrdp::CliParseResult r =
        parse({ QStringLiteral("--host"), QStringLiteral("9000"), QStringLiteral("secret"),
                QStringLiteral("--connect"), QStringLiteral("1.2.3.4:9000"),
                QStringLiteral("pw") });
    QVERIFY2(!r.ok, "两个角色同时给只会在 main 末尾悄悄丢掉一个");
    QVERIFY2(r.error.contains(QStringLiteral("不能同时")), qPrintable(r.error));
}

// 口令不足 6 位 / 含非 ASCII 时必须在**解析阶段**就拒：--host 模式不开窗口，
// 放过去的话 startHost 会拒绝、而进程仍在 app.exec() 里空转（脚本永远等不到结果）。
void TestCliArgs::rejectsWeakHostPassword()
{
    const auto host = [](const QString &pw) {
        return parse({ QStringLiteral("--host"), QStringLiteral("9000"), pw });
    };

    QVERIFY2(!host(QStringLiteral("pw")).ok, "2 位口令应被拒");
    QVERIFY2(host(QStringLiteral("pw")).error.contains(QStringLiteral("口令")),
             qPrintable(host(QStringLiteral("pw")).error));
    QVERIFY2(!host(QStringLiteral("abcde")).ok, "5 位应被拒");
    QVERIFY2(!host(QStringLiteral("abc def")).ok, "含空格应被拒");
    QVERIFY2(!host(QStringLiteral("abc口令")).ok, "非 ASCII 应被拒");

    QVERIFY2(host(QStringLiteral("abcde1")).ok, "6 位纯 ASCII 应当通过");
}

void TestCliArgs::rejectsMultipleActions()
{
    const lanrdp::CliParseResult r =
        parse({ QStringLiteral("--connect"), QStringLiteral("1.2.3.4:9000"),
                QStringLiteral("pw"), QStringLiteral("--quit-on-result"),
                QStringLiteral("--measure-seconds"), QStringLiteral("5") });
    QVERIFY2(!r.ok, "两个动作会互相抢 app.exit()");
    QVERIFY2(r.error.contains(QStringLiteral("只能选一个")), qPrintable(r.error));
}

// 动作都挂在 --connect 上：缺了它，有的被静默忽略（--send-file 等），有的仅装了
// 计时器、把进程留在「等一个永远不会到来的握手」里（--expect-frame）。
void TestCliArgs::rejectsActionWithoutConnect()
{
    const auto expectRejected = [](const QStringList &args) {
        const lanrdp::CliParseResult r = parse(args);
        return !r.ok && r.error.contains(QStringLiteral("--connect"));
    };

    QVERIFY(expectRejected({ QStringLiteral("--expect-frame") }));
    QVERIFY(expectRejected({ QStringLiteral("--measure-seconds"), QStringLiteral("5") }));
    QVERIFY(expectRejected({ QStringLiteral("--send-file"), QStringLiteral("a.bin") }));
    QVERIFY(expectRejected({ QStringLiteral("--quit-on-result") }));

    // 纯被控端模式也不该带动作 —— 那些动作全是控制端握手后才有的行为。
    QVERIFY(expectRejected({ QStringLiteral("--host"), QStringLiteral("9000"),
                             QStringLiteral("secret"), QStringLiteral("--quit-on-result") }));
}

// `--bitrate` 是**唯一**不因取值非法而失败的数字参数：留默认 + 打一条 warning。
void TestCliArgs::invalidBitrateFallsBackToDefault()
{
    const lanrdp::CliParseResult r = parse({ QStringLiteral("--bitrate"),
                                             QStringLiteral("abc") });
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.options.bitrateBps, 8 * 1000 * 1000);
}

QTEST_GUILESS_MAIN(TestCliArgs)
#include "test_cliargs.moc"
