#include "app/SessionStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
// DPAPI。要 windows.h 在前（DATA_BLOB 在 wincrypt.h 里，但它依赖 windows 的基本类型）。
#include <wincrypt.h>

namespace lanrdp {

namespace {

// 端口只有 1..65535 是合法的。ini 是人能改的文件，读到 0 / 70000 / 一堆字母
// 都不奇怪 —— 这里一律退回默认值，而不是把非法值带进 listen()。
quint16 readPort(const QSettings &ini, const QString &key, quint16 fallback)
{
    bool ok = false;
    const uint value = ini.value(key).toUInt(&ok);
    if (!ok || value < 1 || value > 65535)
        return fallback;
    return static_cast<quint16>(value);
}

QString readTrimmed(const QSettings &ini, const QString &key, const QString &fallback)
{
    const QString value = ini.value(key).toString().trimmed();
    return value.isEmpty() ? fallback : value;
}

// 码率上下限。ini 可以被人手改，读到 0 或 10 位数字都不奇怪 —— 越界一律退回默认，
// 而不是把一个荒谬的值带进编码器（`setH264Params` 只兜住了下限 100 kbps）。
constexpr int kMinBitrateBps = 100 * 1000;          // 100 kbps
constexpr int kMaxBitrateBps = 100 * 1000 * 1000;   // 100 Mbps

int readBitrate(const QSettings &ini, const QString &key, int fallback)
{
    bool ok = false;
    const int value = ini.value(key).toInt(&ok);
    if (!ok || value < kMinBitrateBps || value > kMaxBitrateBps)
        return fallback;
    return value;
}

bool ensureParentDir(const QString &filePath)
{
    const QString dir = QFileInfo(filePath).absolutePath();
    return dir.isEmpty() || QDir().mkpath(dir);
}

} // namespace

QString defaultSessionFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QCoreApplication::applicationDirPath();
    return QDir(base).absoluteFilePath(QStringLiteral("lanrdp/session.ini"));
}

QString protectSecret(const QString &plain)
{
    if (plain.isEmpty())
        return QString();

    // QString 内部就是 UTF-16，直接当字节数组交给 DPAPI，省掉一次编码转换 ——
    // 也就省掉了「转换时有损 ⇒ 解出来跟原来不是同一个口令」这类问题。
    //
    // 长度 = size() * sizeof(char16_t) 本来就是精确的，别「修正」成
    // utf16().size() 或 toUtf8().size()：QString::size() 返回的就是 UTF-16
    // **代码单元**数（BMP 外的字符是 surrogate pair，占 2 个单元 ⇒ 4 字节），
    // 正好等于 utf16() 指向的字节数。曾有人报这里是 bug，核实是不成立的。
    const QByteArray raw(reinterpret_cast<const char *>(plain.utf16()),
                         plain.size() * static_cast<int>(sizeof(char16_t)));

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(raw.constData()));
    in.cbData = static_cast<DWORD>(raw.size());

    DATA_BLOB out{};
    // CRYPTPROTECT_UI_FORBIDDEN：不允许系统弹任何 UI。这个函数在保存配置的路径上，
    // 一旦弹窗卡住，用户看到的是「点确定没反应」。
    if (!::CryptProtectData(&in, L"lanrdp session", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return QString();
    }

    const QByteArray cipher(reinterpret_cast<const char *>(out.pbData),
                            static_cast<int>(out.cbData));
    ::LocalFree(out.pbData);
    return QString::fromLatin1(cipher.toBase64());
}

QString unprotectSecret(const QString &stored)
{
    if (stored.isEmpty())
        return QString();

    const QByteArray cipher = QByteArray::fromBase64(stored.toLatin1());
    if (cipher.isEmpty())
        return QString();

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()));
    in.cbData = static_cast<DWORD>(cipher.size());

    DATA_BLOB out{};
    if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return QString();   // 换用户 / 换机器 / 文件被改过 —— 都走这里
    }

    // 明文必须是整数个 UTF-16 码元，否则说明这份密文不是本程序写的。
    if (out.cbData % sizeof(char16_t) != 0) {
        ::LocalFree(out.pbData);
        return QString();
    }

    const QString plain(reinterpret_cast<const QChar *>(out.pbData),
                        static_cast<int>(out.cbData / sizeof(char16_t)));
    ::LocalFree(out.pbData);
    return plain;
}

SessionStore::SessionStore(const QString &filePath)
    : filePath_(filePath)
{
}

HostSession SessionStore::loadHost() const
{
    HostSession session;
    if (!isEnabled())
        return session;

    const QSettings ini(filePath_, QSettings::IniFormat);
    session.port       = readPort(ini, QStringLiteral("host/port"), session.port);
    session.password   = unprotectSecret(ini.value(QStringLiteral("host/password")).toString());
    session.receiveDir = readTrimmed(ini, QStringLiteral("host/receiveDir"), session.receiveDir);
    session.useH264    = ini.value(QStringLiteral("host/useH264"), session.useH264).toBool();
    session.bitrateBps = readBitrate(ini, QStringLiteral("host/bitrateBps"), session.bitrateBps);
    return session;
}

PeerSession SessionStore::loadPeer() const
{
    PeerSession session;
    if (!isEnabled())
        return session;

    const QSettings ini(filePath_, QSettings::IniFormat);
    session.host     = readTrimmed(ini, QStringLiteral("peer/host"), session.host);
    session.port     = readPort(ini, QStringLiteral("peer/port"), session.port);
    session.password = unprotectSecret(ini.value(QStringLiteral("peer/password")).toString());
    return session;
}

QString SessionStore::loadUiLanguage() const
{
    if (!isEnabled())
        return QString();

    const QSettings ini(filePath_, QSettings::IniFormat);
    return ini.value(QStringLiteral("ui/language")).toString().trimmed();
}

bool SessionStore::saveUiLanguage(const QString &key)
{
    if (!isEnabled())
        return false;
    if (!ensureParentDir(filePath_))
        return false;

    // QSettings 写回时会保留同一文件里其他键，所以这里只动 ui/language，
    // 不会把用户已经存好的端口 / 口令冲掉。
    QSettings ini(filePath_, QSettings::IniFormat);
    ini.setValue(QStringLiteral("ui/language"), key);
    ini.sync();
    return ini.status() == QSettings::NoError;
}

// 界面外观（深色 / 浅色）。与语言同规矩：跨会话的用户偏好，不是「启动一次会话的
// 参数」。没存过 → 空串，由调用方按默认（深色）处理 —— 不在这里返回默认键，
// 免得「没选过」和「明确选了深色」从此分不出来。
QString SessionStore::loadUiTheme() const
{
    if (!isEnabled())
        return QString();

    const QSettings ini(filePath_, QSettings::IniFormat);
    return ini.value(QStringLiteral("ui/theme")).toString().trimmed();
}

bool SessionStore::saveUiTheme(const QString &key)
{
    if (!isEnabled())
        return false;
    if (!ensureParentDir(filePath_))
        return false;

    QSettings ini(filePath_, QSettings::IniFormat);
    ini.setValue(QStringLiteral("ui/theme"), key);
    ini.sync();
    return ini.status() == QSettings::NoError;
}

QString appAutoReconnectKey()
{
    return QStringLiteral("app/autoReconnect");
}

QString appReceiveAudioKey()
{
    return QStringLiteral("app/receiveAudio");
}

QString appTrayResidentKey()
{
    return QStringLiteral("app/trayResident");
}

QString appSingleInstanceKey()
{
    return QStringLiteral("app/singleInstance");
}

QString appAutoStartKey()
{
    return QStringLiteral("app/autoStart");
}

// 三态读。`nullopt` = 这个键**从没存过**（首次运行，或是升级前的老 ini）。
//
// `contains()` 是唯一能把「没存过」与「存了 false」分开的判据：`value(key).toBool()`
// 在两种情况下都返回 false，混起来用就再也回不到默认值 —— 而首启套默认正是本次
// 要的行为。值的两种写法（`true/false`、`1/0`）都认，手工改过 ini 也不会读歪。
std::optional<bool> SessionStore::loadAppSwitch(const QString &key) const
{
    if (!isEnabled() || key.isEmpty())
        return std::nullopt;

    const QSettings ini(filePath_, QSettings::IniFormat);
    if (!ini.contains(key))
        return std::nullopt;
    return ini.value(key).toBool();
}

bool SessionStore::saveAppSwitch(const QString &key, bool value)
{
    if (!isEnabled() || key.isEmpty())
        return false;
    if (!ensureParentDir(filePath_))
        return false;

    // 与语言 / 主题同规矩：QSettings 写回保留同一文件里的其他键，
    // 所以勾一下托盘不会把用户存好的端口、口令或主题冲掉。
    QSettings ini(filePath_, QSettings::IniFormat);
    ini.setValue(key, value);
    ini.sync();
    return ini.status() == QSettings::NoError;
}

namespace {

// 把口令写进 ini。返回空串 = 成功；非空 = 失败原因（可直接给用户看）。
//
// 三种情形必须分开处理：
// · 空口令 = 用户主动清空 ⇒ 真的把键清掉（否则旧密文一直留在那儿，用户以为没清成功）
// · 加密失败 ⇒ **一个键都不写**，ini 里保留上一次的密文；绝不能拿空串盖掉 ——
//   那等于把用户上次存好的口令悄悄抹了，而他连提示都不会有
// · 正常 ⇒ 写密文
QString writeSecret(QSettings &ini, const QString &key, const QString &plain)
{
    if (plain.isEmpty())
    {
        ini.setValue(key, QString());
        return QString();
    }

    const QString cipher = protectSecret(plain);
    if (cipher.isEmpty())
        return QStringLiteral("口令加密失败，本次未保存口令");

    ini.setValue(key, cipher);
    return QString();
}

} // namespace

bool SessionStore::saveHost(const HostSession &session, QString *error)
{
    if (error)
        error->clear();
    if (!isEnabled())
    {
        if (error)
            *error = QStringLiteral("配置持久化已停用");
        return false;
    }
    if (!ensureParentDir(filePath_))
    {
        if (error)
            *error = QStringLiteral("无法创建配置目录");
        return false;
    }

    QSettings ini(filePath_, QSettings::IniFormat);
    ini.setValue(QStringLiteral("host/port"), session.port);
    const QString secretError =
        writeSecret(ini, QStringLiteral("host/password"), session.password);
    ini.setValue(QStringLiteral("host/receiveDir"), session.receiveDir);
    ini.setValue(QStringLiteral("host/useH264"), session.useH264);
    ini.setValue(QStringLiteral("host/bitrateBps"), session.bitrateBps);
    ini.sync();
    if (ini.status() != QSettings::NoError)
    {
        if (error)
            *error = QStringLiteral("无法写入配置文件");
        return false;
    }
    if (!secretError.isEmpty())
    {
        // 其余字段已经落盘了，只是口令那一项没成 —— 原因单独报，别混进 IO 失败里。
        if (error)
            *error = secretError;
        return false;
    }
    return true;
}

bool SessionStore::savePeer(const PeerSession &session, QString *error)
{
    if (error)
        error->clear();
    if (!isEnabled())
    {
        if (error)
            *error = QStringLiteral("配置持久化已停用");
        return false;
    }
    if (!ensureParentDir(filePath_))
    {
        if (error)
            *error = QStringLiteral("无法创建配置目录");
        return false;
    }

    QSettings ini(filePath_, QSettings::IniFormat);
    ini.setValue(QStringLiteral("peer/host"), session.host);
    ini.setValue(QStringLiteral("peer/port"), session.port);
    const QString secretError =
        writeSecret(ini, QStringLiteral("peer/password"), session.password);
    ini.sync();
    if (ini.status() != QSettings::NoError)
    {
        if (error)
            *error = QStringLiteral("无法写入配置文件");
        return false;
    }
    if (!secretError.isEmpty())
    {
        if (error)
            *error = secretError;
        return false;
    }
    return true;
}

} // namespace lanrdp
