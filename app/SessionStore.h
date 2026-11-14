#pragma once

#include <QString>

namespace lanrdp {

// 会话启动参数的落盘。
//
// 落盘的是**启动一次会话要填的那几个字段**（端口 / 口令 / 地址 / 接收目录 / 编码），
// 目的是「下次打开程序不用重填」。⚠️ 【设置…】里那两个开关**不在其中**：它们是
// 本次运行的临时偏好，跨会话保留反而会让「我明明关掉过」变成悬案。
//
// 口令**不以明文落盘**：写之前过 Windows DPAPI（`CryptProtectData`），与当前
// Windows 用户绑定。要说实话 —— 它抬高的门槛只是「要解出来，得像这个用户一样
// 登录并调用同一个系统 API」，不是密码学意义上的保护；但至少 ini 被拷走、
// 被其它用户读到、或者被当文本搜到，都解不出原始口令。
//
// ⚠️ 因此 ini **不能跨机拷**：换机器 / 换用户都解不开。解不开当作「没存过」，
// 用户重输一次即可 —— 宁可重输，也不该把绑机器失败变成启动报错。

struct HostSession {
    quint16 port = 9000;
    QString password;
    QString receiveDir;
    bool    useH264 = true;
    // 压缩码率上限（bps）。老版本 ini 里没有这个键，读到默认值即可。
    int     bitrateBps = 8 * 1000 * 1000;
};

struct PeerSession {
    QString host = QStringLiteral("127.0.0.1");
    quint16 port = 9000;
    QString password;
};

// 默认落盘位置：`%LOCALAPPDATA%\lanrdp\session.ini`。
// 刻意**不**放 exe 同目录 —— 程序可能被拷到 `C:\Program Files\`（那里没有普通
// 用户的写权限），也不能假定工作目录；取不到标准的用户目录时才退回 exe 同目录。
QString defaultSessionFilePath();

// 口令的存取。`protectSecret("")` 返回空串（空口令不该被当成"记住了一个空口令"）。
QString protectSecret(const QString &plain);
QString unprotectSecret(const QString &stored);

// ini 的读写。
//
// `filePath` 为空 = **停用**：读写全部不落盘、不访问文件系统。MainWindow 的默认
// 构造就是这么用的，测试与自动化因此天然隔离 —— 它们不会读到、也不会写坏真实
// 的用户配置（让测试默认碰真实配置，是那种"平时看不出来、换台机器才炸"的错误）。
class SessionStore
{
public:
    explicit SessionStore(const QString &filePath = QString());

    bool isEnabled() const { return !filePath_.isEmpty(); }
    QString filePath() const { return filePath_; }

    // 读不到 / 字段非法 / 口令解不开，一律返回该字段的默认值（不抛、不报错）。
    HostSession loadHost() const;
    PeerSession loadPeer() const;

    // 界面语言（键 `ui/language`，取值见 common/I18n.h 的 uiLanguageKey()）。
    //
    // 它和 host/peer 那两组不是一类东西：那些是「启动一次会话要填的参数」，
    // 这个跨会话的用户偏好。放在同一个 ini 里只是为了「一个程序只有一份配置」，
    // 不必再教用户去第二个位置找。
    //
    // 没存过 / 读不到 → 空串，由调用方按「跟随系统」处理。**不返回默认语言的键**：
    // 「没选过」和「明确选了跟随系统」在语义上是两件事，虽然当前表现一样。
    QString loadUiLanguage() const;
    bool saveUiLanguage(const QString &key);

    // 返回是否**完整**写进去了。失败（目录只读 / 磁盘满）由调用方决定怎么提示 ——
    // 这里静默吞掉的话，用户会以为"记住了"，下次开程序发现又空了。
    //
    // `error`（可为空）在返回 false 时填一句能直接给用户看的原因。
    // ⚠️ **口令加密失败也算失败**：那时其余字段照写，但 ini 里的口令键**保持原样**
    //（不会被空串盖掉）。从前这条只判 `QSettings::status()`，加密失败写进去一个空
    // 串照样报成功 —— 用户下次启动发现口令没了，而且没有任何线索。
    bool saveHost(const HostSession &session, QString *error = nullptr);
    bool savePeer(const PeerSession &session, QString *error = nullptr);

private:
    QString filePath_;
};

} // namespace lanrdp
