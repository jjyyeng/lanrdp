#pragma once

#include <QString>
#include <QStringList>

namespace lanrdp {

// 界面语言。
//
// **中文是源语言**：源码里 `tr("…")` 写的就是最终中文，所以中文档不需要任何
// 翻译文件 —— 只要不装 QTranslator，tr() 原样返回源码里的字面量。英文由
// `translations/lanrdp_en.ts` 提供，构建时经 lrelease 编成 qm 并嵌进 exe
// （`:/i18n/lanrdp_en.qm`），运行期用 QTranslator 装上。
//
// 为什么不让英文当源语言：全项目的注释、日志、术语口径都是中文，把界面文案
// 抽成英文再译回中文，等于把「界面说什么」这件事从唯一权威位置搬走一次。
enum class UiLanguage {
    System,    // 跟随系统：中文系统用中文，其余用英文
    Chinese,
    English,
};

// 落盘用的稳定标识。**不要用枚举值直接落盘** —— 将来往枚举中间插一项，
// 老配置里的数字就会指到另一种语言，而那种错没人会想到去查。
QString uiLanguageKey(UiLanguage lang);
UiLanguage uiLanguageFromKey(const QString &key, UiLanguage fallback = UiLanguage::System);

// 把「系统界面语言的候选列表」（BCP 47，按优先级排序）解析成我们支持的两种语言。
//
// ⚠️ 判据是**界面显示语言**，不是区域格式。Windows 上这两者是独立设置，
// 而 `QLocale::system()` 取的是后者（还会被 LANG / LC_ALL 环境变量带偏）：
// 实测区域格式为 en-US 时它返回 en_US，而系统界面语言明明是中文。
// 真正的界面语言在 `QLocale::system().uiLanguages()` 首项里。
//
// 单独抽出来是为了**可测**：真机上这两个值随系统设置变化，测试里喂不出稳定
// 用例，只能直接喂列表。
UiLanguage uiLanguageFromSystemLanguages(const QStringList &uiLanguages);

// 「跟随系统」实际落到哪种语言。判据只看界面语言本身：
// 中文（含繁体 / 各地变体）→ 中文，其余一律英文。
// 之所以不是「非英文一律中文」：日语 / 德语系统上给一屏中文，还不如给英文。
UiLanguage resolveUiLanguage(UiLanguage lang);

// 进程内当前**生效**的语言（已解析过，不会是 System）。
UiLanguage currentUiLanguage();

// 用户**选择**的语言（可能仍是 System）。
// 菜单勾选与落盘都按它 —— 「跟随系统」是一个真实的选择，不能因为系统恰好是
// 中文就把它抹成「用户选了中文」，否则换到英文系统后行为就变了。
UiLanguage uiLanguageChoice();

// 装语言。`lang` 可以是 System（内部先解析）。
//
// 返回值 = 界面**是否真的**成了目标语言：英文资源缺失（qm 没嵌进去、资源损坏）
// 时返回 false，界面停在中文。调用方据此提示，但不必把它当致命错误 ——
// 语言没切成功，程序照常能用。
//
// 切换会卸载旧的 QTranslator 再装新的，两次操作各自触发 QEvent::LanguageChange；
// 已创建的控件要在这个事件里重刷自己的文案（Qt 会把事件递归发给所有子控件，
// 但 QAction 之类的非控件对象收不到，得由宿主手动重刷）。
bool setUiLanguage(UiLanguage lang);

// 英文翻译资源是否存在。用于区分「语言没切成功」与「本来就要中文」。
bool hasEnglishTranslation();

// 非 QObject 的辅助函数（自由函数、inline 工具）翻译时用的上下文名：`"lanrdp"`。
//
// ⚠️ 必须**直接写成字面量**：`QCoreApplication::translate("lanrdp", "…")`。
// lupdate 是源码扫描器，不解析常量 —— 写成 `translate(kTranslationContext, …)`
// 的条目会整批漏抽（症状是英文界面里这一句仍是中文，而 .ts 里根本查不到它）。
// 所以这里只留约定、不留常量：一个能写错的名字，不如没有这个名字。

} // namespace lanrdp
