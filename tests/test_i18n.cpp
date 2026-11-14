#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QLocale>
#include <QTemporaryDir>
#include <QWidget>

#include "app/SessionStore.h"
#include "common/I18n.h"

// 界面语言：切换机制与落盘。
//
// 值得单独钉的理由：语言是**进程级全局状态**，一旦错了，症状遍布整个界面
// （菜单英文、状态栏中文、tooltip 又是第三种），靠肉眼走查根本覆盖不完。
// 这里钉的是「机制」——枚举与落盘键的往返、qm 是否真的嵌进了 exe、
// 切换后 translate() 与 changeEvent 是否真的按预期动作。
// 逐条文案翻得对不对不归测试管（那是 .ts 的内容），只钉「有没有英文」。
class TestI18n : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void keyRoundTrip();
    void unknownKeyFallsBackToGivenDefault();
    void systemResolvesToConcreteLanguage();
    void systemLanguagesMapToChineseOrEnglish();
    void englishResourceIsEmbedded();
    void englishTranslatesKnownText();
    void switchingBackToChineseReturnsSourceText();
    void widgetsReceiveLanguageChange();
    void languageChangeReachesChildWidgets();
    void sessionStorePersistsLanguage();
};

void TestI18n::initTestCase()
{
    // 起点固定成中文：这台机器以外的环境（英文 Windows）不该让本套件的前提漂移。
    lanrdp::setUiLanguage(lanrdp::UiLanguage::Chinese);
}

void TestI18n::cleanupTestCase()
{
    // 全局状态要还给「跟随系统」，否则同一进程里后面的用例会看到被改过的语言。
    lanrdp::setUiLanguage(lanrdp::UiLanguage::System);
}

// 落盘键必须是稳定字符串，不是枚举值：往枚举中间插一项不该改变老配置的含义。
void TestI18n::keyRoundTrip()
{
    QCOMPARE(lanrdp::uiLanguageKey(lanrdp::UiLanguage::System), QStringLiteral("system"));
    QCOMPARE(lanrdp::uiLanguageKey(lanrdp::UiLanguage::Chinese), QStringLiteral("zh"));
    QCOMPARE(lanrdp::uiLanguageKey(lanrdp::UiLanguage::English), QStringLiteral("en"));

    for (auto lang : {lanrdp::UiLanguage::System, lanrdp::UiLanguage::Chinese,
                      lanrdp::UiLanguage::English}) {
        QCOMPARE(lanrdp::uiLanguageFromKey(lanrdp::uiLanguageKey(lang)), lang);
    }

    // ini 是用户能手改的：大小写与空白不该让程序掉进「未知值」。
    QCOMPARE(lanrdp::uiLanguageFromKey(QStringLiteral(" EN ")), lanrdp::UiLanguage::English);
}

void TestI18n::unknownKeyFallsBackToGivenDefault()
{
    QCOMPARE(lanrdp::uiLanguageFromKey(QStringLiteral("ja"), lanrdp::UiLanguage::Chinese),
             lanrdp::UiLanguage::Chinese);
    QCOMPARE(lanrdp::uiLanguageFromKey(QString(), lanrdp::UiLanguage::System),
             lanrdp::UiLanguage::System);
}

// 「跟随系统」必须解析成具体语言 —— 界面上不存在「跟随系统」这种语言。
void TestI18n::systemResolvesToConcreteLanguage()
{
    const lanrdp::UiLanguage resolved = lanrdp::resolveUiLanguage(lanrdp::UiLanguage::System);
    QVERIFY(resolved == lanrdp::UiLanguage::Chinese || resolved == lanrdp::UiLanguage::English);

    // 显式选择的语言不该被系统语言覆盖。
    QCOMPARE(lanrdp::resolveUiLanguage(lanrdp::UiLanguage::English),
             lanrdp::UiLanguage::English);
    QCOMPARE(lanrdp::resolveUiLanguage(lanrdp::UiLanguage::Chinese),
             lanrdp::UiLanguage::Chinese);
}

// 「跟随系统」的判据必须锚在**界面显示语言**上，而不是区域格式。
//
// 抽成纯函数就是为了能钉这条：真机上 QLocale::system() 在 Windows 取的是区域
// 格式，还会被 LANG / LC_ALL 带偏（实测同一台机器：区域格式 en-US 时它报
// en_US，而系统界面语言是中文）。不喂固定列表就永远测不出这层区别。
void TestI18n::systemLanguagesMapToChineseOrEnglish()
{
    using lanrdp::UiLanguage;

    const auto langs = [](std::initializer_list<const char *> list) {
        QStringList out;
        for (const char *tag : list)
            out.append(QString::fromLatin1(tag));
        return out;
    };

    // 中文的各种变体都算中文：简体 / 繁体 / 中国香港 / 中国台湾 / 新加坡
    QCOMPARE(lanrdp::uiLanguageFromSystemLanguages(langs({ "zh-Hans-CN", "zh-CN", "zh" })),
             UiLanguage::Chinese);
    QCOMPARE(lanrdp::uiLanguageFromSystemLanguages(langs({ "zh-TW", "zh-Hant" })),
             UiLanguage::Chinese);
    QCOMPARE(lanrdp::uiLanguageFromSystemLanguages(langs({ "zh-HK" })), UiLanguage::Chinese);

    // 英文系统就是英文
    QCOMPARE(lanrdp::uiLanguageFromSystemLanguages(langs({ "en-US", "en-Latn-US", "en" })),
             UiLanguage::English);

    // 既非中文也非英文（日语 / 德语 …）：给英文，不给中文
    QCOMPARE(lanrdp::uiLanguageFromSystemLanguages(langs({ "ja-JP", "ja" })), UiLanguage::English);

    // 拿不到列表：按英文兜底
    QCOMPARE(lanrdp::uiLanguageFromSystemLanguages(QStringList()), UiLanguage::English);
}

// 英文翻译是**编进 exe 的资源**，不是旁边的一个文件 ——
// 这条断言防的是「构建时忘了嵌 qm」，那种情况在开发机上一切正常，
// 换台机器就只剩中文。
void TestI18n::englishResourceIsEmbedded()
{
    QVERIFY2(lanrdp::hasEnglishTranslation(),
             "exe 里没有 :/i18n/lanrdp_en.qm —— 检查 CMake 的 qt_add_translations");
}

void TestI18n::englishTranslatesKnownText()
{
    QVERIFY(lanrdp::setUiLanguage(lanrdp::UiLanguage::English));
    QCOMPARE(lanrdp::currentUiLanguage(), lanrdp::UiLanguage::English);
    QCOMPARE(lanrdp::uiLanguageChoice(), lanrdp::UiLanguage::English);

    // 上下文 = QObject 类名；自由函数用的是 "lanrdp"（见 common/I18n.h）。
    QCOMPARE(QCoreApplication::translate("MainWindow", "被控"), QStringLiteral("Hosting"));
    QCOMPARE(QCoreApplication::translate("lanrdp", "%1 — 等待连接"),
             QStringLiteral("%1 — waiting for connections"));
}

void TestI18n::switchingBackToChineseReturnsSourceText()
{
    QVERIFY(lanrdp::setUiLanguage(lanrdp::UiLanguage::English));
    QVERIFY(lanrdp::setUiLanguage(lanrdp::UiLanguage::Chinese));

    QCOMPARE(lanrdp::currentUiLanguage(), lanrdp::UiLanguage::Chinese);
    // 中文是源语言：不装翻译器时 tr()/translate() 原样返回源码里的字面量。
    QCOMPARE(QCoreApplication::translate("MainWindow", "被控"), QStringLiteral("被控"));
    QCOMPARE(QCoreApplication::translate("lanrdp", "%1 — 等待连接"),
             QStringLiteral("%1 — 等待连接"));
}

// 切换语言靠 QEvent::LanguageChange 让已存在的控件自己重刷文案。
// 这条钉的是「事件真的发出去了」—— 漏了它，切换后旧窗口会一直停在旧语言。
class LanguageChangeProbe : public QWidget
{
public:
    explicit LanguageChangeProbe(QWidget *parent = nullptr) : QWidget(parent) {}
    int changes = 0;

protected:
    void changeEvent(QEvent *event) override
    {
        if (event->type() == QEvent::LanguageChange)
            ++changes;
        QWidget::changeEvent(event);
    }
};

void TestI18n::widgetsReceiveLanguageChange()
{
    LanguageChangeProbe probe;
    probe.show();

    QVERIFY(lanrdp::setUiLanguage(lanrdp::UiLanguage::English));
    // LanguageChange 是**投递**给顶层控件的（不是当场回调），所以要转一圈事件循环
    // 才能观察到它。生产代码里用户点完菜单自然就回到事件循环了，不必特殊处理。
    QCoreApplication::processEvents();
    QVERIFY2(probe.changes > 0, "装翻译器没有把 LanguageChange 发给顶层控件");

    const int afterEnglish = probe.changes;
    QVERIFY(lanrdp::setUiLanguage(lanrdp::UiLanguage::Chinese));
    QCoreApplication::processEvents();
    QVERIFY2(probe.changes > afterEnglish, "卸翻译器同样要通知界面重刷");
}

// 派发范围到**子控件**为止（Qt 6.11.2 实测）。
//
// 这条为什么值得钉：Qt 文档只说「发给顶层 widget」，很容易据此以为子控件收不到、
// 于是给每个子控件在父类里手写一遍转发（多一层维护，还容易漏）。
// 实测反证：子控件**照样收到**，所以各控件自己的 changeEvent 直接就能用 ——
// 悬浮条 / 空态 / 渲染器都是靠这条成立的。反过来，若哪天升级 Qt 变了行为，
// 这条会立刻转红，而不是等到用户切语言时发现「一屏中文一屏英文」。
void TestI18n::languageChangeReachesChildWidgets()
{
    LanguageChangeProbe top;
    LanguageChangeProbe child(&top);
    top.show();
    child.show();

    QVERIFY(lanrdp::setUiLanguage(lanrdp::UiLanguage::English));
    QCoreApplication::processEvents();

    QCOMPARE(top.changes, 1);
    QCOMPARE(child.changes, 1);

    QVERIFY(lanrdp::setUiLanguage(lanrdp::UiLanguage::Chinese));
    QCoreApplication::processEvents();
    QCOMPARE(top.changes, 2);
    QCOMPARE(child.changes, 2);
}

// 语言偏好落在会话 ini 的 ui/language 键里，与端口 / 口令同一个文件。
void TestI18n::sessionStorePersistsLanguage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("session.ini"));

    lanrdp::SessionStore store(path);
    QVERIFY(store.isEnabled());

    // 没存过 = 空串（「没选过」与「选了跟随系统」是两件事）。
    QVERIFY(store.loadUiLanguage().isEmpty());

    QVERIFY(store.saveUiLanguage(lanrdp::uiLanguageKey(lanrdp::UiLanguage::English)));
    QCOMPARE(store.loadUiLanguage(), QStringLiteral("en"));

    // 写语言不能顺手把已经存好的会话参数冲掉（QSettings 会重写整个文件）。
    lanrdp::HostSession host;
    host.port = 9123;
    QVERIFY(store.saveHost(host));
    QVERIFY(store.saveUiLanguage(lanrdp::uiLanguageKey(lanrdp::UiLanguage::Chinese)));

    const lanrdp::HostSession reloaded = store.loadHost();
    QCOMPARE(reloaded.port, quint16(9123));
    QCOMPARE(store.loadUiLanguage(), QStringLiteral("zh"));
}

QTEST_MAIN(TestI18n)
#include "test_i18n.moc"
