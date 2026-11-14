#include "common/I18n.h"

#include <QCoreApplication>
#include <QFile>
#include <QLocale>
#include <QTranslator>

namespace lanrdp {

namespace {

// 进程内当前生效的具体语言（不会存 System）。默认中文 —— 与「不装翻译器时的
// 自然行为」一致，这样即使 QCoreApplication 还没建好，界面也不会突然变英文。
UiLanguage g_current = UiLanguage::Chinese;

// 用户的选择（可能是 System）。与 g_current 分开存：勾选态与落盘读这个，
// 界面文案读 g_current。
UiLanguage g_choice = UiLanguage::System;

// 英文翻译器。故意**不析构**：QTranslator 是 QObject，静态对象会在
// QCoreApplication 之后析构，那时 Qt 的全局状态已经拆了一半。让它活到进程结束
// 只占几十字节，比在退出路径上留一个 order-of-destruction 隐患划算。
QTranslator *englishTranslator()
{
    static QTranslator *translator = new QTranslator();
    return translator;
}

constexpr const char *kEnglishQmPath = ":/i18n/lanrdp_en.qm";

} // namespace

QString uiLanguageKey(UiLanguage lang)
{
    switch (lang) {
    case UiLanguage::Chinese:
        return QStringLiteral("zh");
    case UiLanguage::English:
        return QStringLiteral("en");
    case UiLanguage::System:
        break;
    }
    return QStringLiteral("system");
}

UiLanguage uiLanguageFromKey(const QString &key, UiLanguage fallback)
{
    const QString normalized = key.trimmed().toLower();
    if (normalized == QLatin1String("zh"))
        return UiLanguage::Chinese;
    if (normalized == QLatin1String("en"))
        return UiLanguage::English;
    if (normalized == QLatin1String("system"))
        return UiLanguage::System;
    return fallback;
}

UiLanguage uiLanguageFromSystemLanguages(const QStringList &uiLanguages)
{
    for (const QString &tag : uiLanguages) {
        if (tag.isEmpty())
            continue;
        const QLocale locale(tag);
        if (locale.language() == QLocale::Chinese)
            return UiLanguage::Chinese;
        if (locale.language() == QLocale::English)
            return UiLanguage::English;
    }
    // 既不是中文也不是英文的系统（日语 / 德语 …）：给英文比给一屏中文合适。
    return UiLanguage::English;
}

// 「跟随系统」不认国家/地区，只认语言：中国香港 / 中国台湾 / 新加坡的中文系统
// 都该看到中文界面。反过来，日语、德语系统给中文界面不如给英文 —— 所以要的是
// 「是中文就用中文」，而不是「不是英文就用中文」。
UiLanguage resolveUiLanguage(UiLanguage lang)
{
    if (lang != UiLanguage::System)
        return lang;

    // ⚠️ 必须用 uiLanguages()，不能用 QLocale::system().language()：
    // 后者在 Windows 上取的是**区域格式**，且受 LANG / LC_ALL 影响。
    // 实测同一台机器上两种结果会打架（区域格式 en-US ⇒ en_US，而系统
    // 界面语言是中文），照它判断会让「跟随系统」在中文系统上显示英文。
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    if (!uiLanguages.isEmpty())
        return uiLanguageFromSystemLanguages(uiLanguages);

    // 拿不到列表（极早期 / 无 MUI 信息）：退回区域判断。
    return QLocale::system().language() == QLocale::Chinese ? UiLanguage::Chinese
                                                            : UiLanguage::English;
}

UiLanguage currentUiLanguage()
{
    return g_current;
}

UiLanguage uiLanguageChoice()
{
    return g_choice;
}

bool hasEnglishTranslation()
{
    static const bool present = QFile::exists(QString::fromLatin1(kEnglishQmPath));
    return present;
}

bool setUiLanguage(UiLanguage lang)
{
    const UiLanguage target = resolveUiLanguage(lang);
    g_choice = lang;

    QCoreApplication *app = QCoreApplication::instance();
    if (!app) {
        // 没有应用对象（构造期 / 极早期的测试）：只记下目标，等有人装的时候再装。
        g_current = target;
        return target != UiLanguage::English || hasEnglishTranslation();
    }

    QTranslator *translator = englishTranslator();

    // 先落「当前语言」，再装卸。installTranslator() 是**同步**派发
    // QEvent::LanguageChange 的，重刷文案的代码会在这一行里就跑起来，
    // 那时读 currentUiLanguage() 必须已经是新值。
    g_current = target;

    // 卸载是幂等的：没装着也不会出错。
    app->removeTranslator(translator);

    if (target != UiLanguage::English) {
        // 中文是源语言，不需要翻译文件 —— 卸掉英文器就是中文。
        return true;
    }

    if (!translator->load(QString::fromLatin1(kEnglishQmPath))) {
        // 资源缺失 / 损坏：界面停在中文。选择也一并退回中文 —— 勾着 English
        // 却显示中文，比「没切成」更让人迷惑（会以为是程序坏了）。
        // 调用方据返回值提示原因，不必自己再回滚界面。
        g_current = UiLanguage::Chinese;
        g_choice  = UiLanguage::Chinese;
        return false;
    }

    app->installTranslator(translator);
    return true;
}

} // namespace lanrdp
