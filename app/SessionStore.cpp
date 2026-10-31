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

bool SessionStore::saveHost(const HostSession &session)
{
    if (!isEnabled())
        return false;
    if (!ensureParentDir(filePath_))
        return false;

    QSettings ini(filePath_, QSettings::IniFormat);
    ini.setValue(QStringLiteral("host/port"), session.port);
    ini.setValue(QStringLiteral("host/password"), protectSecret(session.password));
    ini.setValue(QStringLiteral("host/receiveDir"), session.receiveDir);
    ini.setValue(QStringLiteral("host/useH264"), session.useH264);
    ini.sync();
    return ini.status() == QSettings::NoError;
}

bool SessionStore::savePeer(const PeerSession &session)
{
    if (!isEnabled())
        return false;
    if (!ensureParentDir(filePath_))
        return false;

    QSettings ini(filePath_, QSettings::IniFormat);
    ini.setValue(QStringLiteral("peer/host"), session.host);
    ini.setValue(QStringLiteral("peer/port"), session.port);
    ini.setValue(QStringLiteral("peer/password"), protectSecret(session.password));
    ini.sync();
    return ini.status() == QSettings::NoError;
}

} // namespace lanrdp
