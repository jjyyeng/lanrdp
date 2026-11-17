#include "app/AutoStart.h"

#include <QDir>
#include <QSettings>
#include <QtTest>

// 开机自启：写 / 删 HKCU 的 Run 值，以及「程序搬家后路径要跟上」。
//
// ⚠️ 每条用例都写进 `HKCU\Software\lanrdp-test\Run`，**绝不碰**真实的
// `...\CurrentVersion\Run` —— 单测往用户真实自启列表里塞一条，是那种
// 「跑完测试没人发现、重启才看见」的脏副作用。
class TestAutoStart : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // 打开时写进去的是「带引号的绝对路径」：路径里有空格，不加引号就是两个参数
    void enablingWritesQuotedExePath();
    void disablingRemovesTheValue();
    // 目录一挪，Run 键里的旧绝对路径就指向不存在的文件（症状是「开了却再没自启过」）
    void refreshFollowsANewExePath();
    // 关着的时候 refresh 不许顺手把它打开
    void refreshLeavesDisabledStateAlone();
    void storedCommandIsEmptyWhenNeverEnabled();
    // 空路径写进 Run 等于一条无效自启项，必须在入口就拒掉
    void enablingWithEmptyPathIsRefused();
    // 默认键必须是用户级 HKCU：写 HKLM 要管理员，普通用户装不上就没意义
    void defaultKeyIsUnderCurrentUser();
};

namespace {

// 测试专用的键路径（与真实自启键隔离）。出处只有 AutoStart 一处 ——
// 测试和 MainWindow 必须往同一个键写，否则「测了但没测到」。
QString testRunKey()
{
    return lanrdp::AutoStart::testRunKeyPath();
}

QString tidy(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

} // namespace

void TestAutoStart::init()
{
    QSettings settings(testRunKey(), QSettings::NativeFormat);
    settings.clear();
    settings.sync();
}

void TestAutoStart::cleanup()
{
    QSettings settings(testRunKey(), QSettings::NativeFormat);
    settings.clear();
    settings.sync();
}

void TestAutoStart::enablingWritesQuotedExePath()
{
    const QString exe = QStringLiteral("C:/Program Files/lanrdp/lanrdp.exe");
    lanrdp::AutoStart autoStart(testRunKey());

    QString why;
    QVERIFY2(autoStart.setEnabled(true, exe, &why), qPrintable(why));

    QVERIFY(autoStart.isEnabled());
    const QString expected = QLatin1Char('"') + tidy(exe) + QLatin1Char('"');
    QCOMPARE(autoStart.storedCommand(), expected);
}

void TestAutoStart::disablingRemovesTheValue()
{
    const QString exe = QStringLiteral("D:/lanrdp-live/lanrdp.exe");
    lanrdp::AutoStart autoStart(testRunKey());

    QString why;
    QVERIFY2(autoStart.setEnabled(true, exe, &why), qPrintable(why));
    QVERIFY(autoStart.isEnabled());

    QVERIFY2(autoStart.setEnabled(false, exe, &why), qPrintable(why));
    QVERIFY(!autoStart.isEnabled());
    QVERIFY(autoStart.storedCommand().isEmpty());
}

void TestAutoStart::refreshFollowsANewExePath()
{
    lanrdp::AutoStart autoStart(testRunKey());
    const QString before = QStringLiteral("D:/old-place/lanrdp.exe");
    const QString after  = QStringLiteral("E:/new place/lanrdp.exe");

    QString why;
    QVERIFY2(autoStart.setEnabled(true, before, &why), qPrintable(why));
    autoStart.refresh(after);

    QVERIFY(autoStart.isEnabled());
    QVERIFY2(autoStart.storedCommand().contains(tidy(after)),
             qPrintable(autoStart.storedCommand()));
    QVERIFY2(!autoStart.storedCommand().contains(tidy(before)),
             qPrintable(autoStart.storedCommand()));
}

void TestAutoStart::refreshLeavesDisabledStateAlone()
{
    lanrdp::AutoStart autoStart(testRunKey());
    QVERIFY(!autoStart.isEnabled());

    autoStart.refresh(QStringLiteral("D:/lanrdp-live/lanrdp.exe"));

    QVERIFY2(!autoStart.isEnabled(), "refresh must never turn auto-start on");
    QVERIFY(autoStart.storedCommand().isEmpty());
}

void TestAutoStart::storedCommandIsEmptyWhenNeverEnabled()
{
    lanrdp::AutoStart autoStart(testRunKey());
    QVERIFY(!autoStart.isEnabled());
    QVERIFY(autoStart.storedCommand().isEmpty());
}

void TestAutoStart::enablingWithEmptyPathIsRefused()
{
    lanrdp::AutoStart autoStart(testRunKey());
    QString why;

    QVERIFY(!autoStart.setEnabled(true, QString(), &why));
    QVERIFY2(!why.isEmpty(), "refusal must explain why");
    QVERIFY(!autoStart.isEnabled());
}

void TestAutoStart::defaultKeyIsUnderCurrentUser()
{
    const QString key = lanrdp::AutoStart::defaultRunKeyPath();
    QVERIFY2(key.startsWith(QStringLiteral("HKEY_CURRENT_USER"), Qt::CaseInsensitive),
             qPrintable(key));
    QVERIFY2(key.contains(QStringLiteral("CurrentVersion"), Qt::CaseInsensitive),
             qPrintable(key));
    QVERIFY2(key.endsWith(QStringLiteral("Run"), Qt::CaseInsensitive), qPrintable(key));
}

QTEST_GUILESS_MAIN(TestAutoStart)
#include "test_autostart.moc"
