#include "app/AutoStart.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace lanrdp {

namespace {

// Run 键下那一条的值名。出现在注册表编辑器和任务管理器的「启动」页上，
// 所以取程序名而不是类名。
constexpr auto kValueName = "lanrdp";

} // namespace

AutoStart::AutoStart(const QString &runKeyPath)
    : runKeyPath_(runKeyPath)
{
}

QString AutoStart::defaultRunKeyPath()
{
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
}

QString AutoStart::testRunKeyPath()
{
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\lanrdp-test\\Run");
}

QString AutoStart::storedCommand() const
{
    QSettings settings(runKeyPath_, QSettings::NativeFormat);
    return settings.value(QLatin1String(kValueName)).toString().trimmed();
}

bool AutoStart::isEnabled() const
{
    return !storedCommand().isEmpty();
}

bool AutoStart::setEnabled(bool on, const QString &exePath, QString *why)
{
    // 空命令写进 Run 键 = 一条永远不会起作用的自启项，且从界面上看还「开着」。
    // 在入口就拒掉，比让它静默存在好。
    if (on && exePath.trimmed().isEmpty()) {
        if (why)
            *why = QCoreApplication::translate("lanrdp",
                                               "程序路径为空，无法登记开机自启。");
        return false;
    }

    QSettings settings(runKeyPath_, QSettings::NativeFormat);
    if (on) {
        // 一律加引号：`C:\Program Files\...` 不加引号会被拆成两个参数。
        // 没有空格的路径加了引号也是合法的。
        const QString command = QLatin1Char('"') + QDir::toNativeSeparators(exePath)
                              + QLatin1Char('"');
        settings.setValue(QLatin1String(kValueName), command);
    } else {
        settings.remove(QLatin1String(kValueName));
    }
    settings.sync();

    if (settings.status() != QSettings::NoError) {
        if (why)
            *why = QCoreApplication::translate(
                "lanrdp", "写入注册表失败（可能被安全软件拦截）。");
        return false;
    }
    return true;
}

void AutoStart::refresh(const QString &exePath)
{
    // 没启用时什么都不做 —— refresh 的职责是「让已有那条跟上新路径」，
    // 不是「顺手把自启打开」。
    if (!isEnabled())
        return;

    QString ignored;
    setEnabled(true, exePath, &ignored);
}

} // namespace lanrdp
