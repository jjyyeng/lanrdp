#include "app/CliArgs.h"

#include "app/PasswordPolicy.h"

#include <QDebug>

namespace lanrdp {

namespace {

const char *kUsage =
    "用法：\n"
    "  lanrdp                                   普通 GUI 模式（手动点按钮）\n"
    "  lanrdp --host <port> <password> [--raw] [--bitrate <bps>] [--recv-dir <dir>] [--quiet]\n"
    "                                           作为被控端监听（口令至少 6 位、仅英文字符；"
    "--quiet 不显示窗口）\n"
    "  lanrdp --connect <ip:port> <password> <动作> [--quiet]\n"
    "     动作：\n"
    "       --quit-on-result                     握手后打印结果并退出\n"
    "       --expect-frame [ms]                  等第一帧画面，默认 15000 ms\n"
    "       --measure-seconds <n>                接收 n 秒并打印帧率 / 带宽\n"
    "       --send-input <nx> <ny>               拿到操作权后发一次鼠标移动\n"
    "       --send-file <path>                   把本地文件发给对端\n"
    "       --hold-token <ms>                    持有操作权并保持 N ms\n"
    "     可选：--send-wait <ms>（默认 400）、--token-wait <ms>（默认 2000）\n"
    "  lanrdp --cursor-pos                      打印当前光标位置后退出\n"
    "  lanrdp --inject-local <nx> <ny>          不经网络，直接向本机注入一次鼠标移动\n"
    "  [--log-file <path>]                      任意模式可加：日志落盘到指定文件\n"
    "                                           （默认 exe 同目录的 lanrdp.log）\n";

// 解析十进制无符号数。失败（空串 / 含非数字 / 越界）时返回 false 并保持 out 不变。
bool parseUShort(const QString &text, quint16 *out)
{
    bool             ok    = false;
    const uint       value = text.toUInt(&ok);
    if (!ok || value > 0xFFFFu)
        return false;
    *out = static_cast<quint16>(value);
    return true;
}

} // namespace

const char *cliUsage()
{
    return kUsage;
}

CliParseResult parseCliArgs(const QStringList &args)
{
    CliParseResult r;
    CliOptions    &o = r.options;

    QString error;

    for (int i = 1; i < args.size() && error.isEmpty(); ++i) {
        const QString a = args.at(i);

        // 取下一个参数（并前移 i）。没有下一个取值时返回 nullptr，由调用方统一报错 ——
        // 从前条件里的 `&& i + 1 < args.size()` 落空后会被当成「未知参数」静默忽略，
        // 于是 `--host 9000`（漏口令）会连端口一起丢掉，直接进 GUI。
        const auto nextValue = [&]() -> const QString * {
            if (i + 1 >= args.size())
                return nullptr;
            return &args.at(++i);
        };

        if (a == QLatin1String("--host")) {
            const QString *portText     = nextValue();
            const QString *passwordText = nextValue();
            if (!portText || !passwordText) {
                error = QStringLiteral("--host 需要 <port> <password> 两个取值");
                break;
            }
            o.hostMode = true;
            if (!parseUShort(*portText, &o.hostPort)) {
                error = QStringLiteral("--host 的端口非法：%1").arg(*portText);
                break;
            }
            // 与对话框同一条规则（app/PasswordPolicy.h）。这里必须拦：此模式不开窗口，
            // 让它过去的话 startHost 会拒绝、而进程仍在 app.exec() 里空转。
            if (!isValidPassword(*passwordText)) {
                error = QStringLiteral(
                    "--host 的口令至少 6 位，且只能用英文字母、数字和符号");
                break;
            }
            o.password = *passwordText;
        } else if (a == QLatin1String("--connect")) {
            const QString *endpointText = nextValue();
            const QString *passwordText = nextValue();
            if (!endpointText || !passwordText) {
                error = QStringLiteral("--connect 需要 <ip:port> <password> 两个取值");
                break;
            }
            o.connectMode = true;
            const int colon = endpointText->lastIndexOf(QLatin1Char(':'));
            if (colon > 0) {
                o.peerHost = endpointText->left(colon);
                if (!parseUShort(endpointText->mid(colon + 1), &o.peerPort)) {
                    error = QStringLiteral("--connect 的端口非法：%1").arg(*endpointText);
                    break;
                }
            } else {
                o.peerHost = *endpointText;
            }
            o.password = *passwordText;
        } else if (a == QLatin1String("--expect-frame")) {
            o.expectFrameMs = 15000;
            // 取值可选：后面若跟着纯数字就吃掉，否则（如 `--quiet`）保留默认。
            if (i + 1 < args.size() && !args.at(i + 1).startsWith(QLatin1String("--"))) {
                bool      okNumber = false;
                const int value    = args.at(i + 1).toInt(&okNumber);
                if (!okNumber || value <= 0) {
                    error = QStringLiteral("--expect-frame 的取值非法：%1").arg(args.at(i + 1));
                    break;
                }
                o.expectFrameMs = value;
                ++i;
            }
        } else if (a == QLatin1String("--measure-seconds")) {
            const QString *text  = nextValue();
            bool           ok    = false;
            const int      value = text ? text->toInt(&ok) : 0;
            if (!text || !ok || value <= 0) {
                error = QStringLiteral("--measure-seconds 的取值非法：%1")
                            .arg(text ? *text : QStringLiteral("（缺）"));
                break;
            }
            o.measureSeconds = value;
        } else if (a == QLatin1String("--send-input")) {
            const QString *nxText = nextValue();
            const QString *nyText = nextValue();
            if (!nxText || !nyText) {
                error = QStringLiteral("--send-input 需要 <nx> <ny> 两个取值");
                break;
            }
            o.sendInput = true;
            if (!parseUShort(*nxText, &o.sendNx)) {
                error = QStringLiteral("--send-input 的 nx 非法：%1").arg(*nxText);
                break;
            }
            if (!parseUShort(*nyText, &o.sendNy)) {
                error = QStringLiteral("--send-input 的 ny 非法：%1").arg(*nyText);
                break;
            }
        } else if (a == QLatin1String("--send-wait")) {
            const QString *text  = nextValue();
            bool           ok    = false;
            const int      value = text ? text->toInt(&ok) : 0;
            if (!text || !ok || value < 0) {
                error = QStringLiteral("--send-wait 的取值非法：%1")
                            .arg(text ? *text : QStringLiteral("（缺）"));
                break;
            }
            o.sendWaitMs = value;
        } else if (a == QLatin1String("--hold-token")) {
            const QString *text  = nextValue();
            bool           ok    = false;
            const int      value = text ? text->toInt(&ok) : 0;
            if (!text || !ok || value <= 0) {
                error = QStringLiteral("--hold-token 的取值非法：%1")
                            .arg(text ? *text : QStringLiteral("（缺）"));
                break;
            }
            o.holdTokenMs = value;
        } else if (a == QLatin1String("--token-wait")) {
            const QString *text  = nextValue();
            bool           ok    = false;
            const int      value = text ? text->toInt(&ok) : 0;
            if (!text || !ok || value <= 0) {
                error = QStringLiteral("--token-wait 的取值非法：%1")
                            .arg(text ? *text : QStringLiteral("（缺）"));
                break;
            }
            o.tokenWaitMs = value;
        } else if (a == QLatin1String("--inject-local")) {
            const QString *nxText = nextValue();
            const QString *nyText = nextValue();
            if (!nxText || !nyText) {
                error = QStringLiteral("--inject-local 需要 <nx> <ny> 两个取值");
                break;
            }
            o.injectLocal = true;
            if (!parseUShort(*nxText, &o.localNx)) {
                error = QStringLiteral("--inject-local 的 nx 非法：%1").arg(*nxText);
                break;
            }
            if (!parseUShort(*nyText, &o.localNy)) {
                error = QStringLiteral("--inject-local 的 ny 非法：%1").arg(*nyText);
                break;
            }
        } else if (a == QLatin1String("--cursor-pos")) {
            o.cursorPosOnly = true;
        } else if (a == QLatin1String("--raw")) {
            o.useRawFrames = true;   // 被控端发裸帧（对照测试用）
        } else if (a == QLatin1String("--recv-dir")) {
            const QString *text = nextValue();
            if (!text) {
                error = QStringLiteral("--recv-dir 缺少取值");
                break;
            }
            o.recvDir = *text;
        } else if (a == QLatin1String("--send-file")) {
            const QString *text = nextValue();
            if (!text) {
                error = QStringLiteral("--send-file 缺少取值");
                break;
            }
            o.sendFilePath = *text;
        } else if (a == QLatin1String("--log-file")) {
            const QString *text = nextValue();
            if (!text) {
                error = QStringLiteral("--log-file 缺少取值");
                break;
            }
            o.logFilePath = *text;
        } else if (a == QLatin1String("--bitrate")) {
            const QString *text = nextValue();
            if (!text) {
                error = QStringLiteral("--bitrate 缺少取值");
                break;
            }
            // 解析失败就**保留默认**，不要把 0 当成
            // 「用户想要 0 码率」传下去 —— 0 会被 setH264Params 的下限静默抬成
            // 100 kbps，用户敲错一个字符却看不出任何异常。
            bool      okBitrate = false;
            const int parsed    = text->toInt(&okBitrate);
            if (okBitrate && parsed > 0)
                o.bitrateBps = parsed;
            else
                qWarning() << QStringLiteral("--bitrate 参数无效，沿用默认 ") << o.bitrateBps
                           << QStringLiteral(" bps（收到：") << *text << QStringLiteral("）");
        } else if (a == QLatin1String("--quiet")) {
            o.quiet = true;
        } else if (a == QLatin1String("--quit-on-result")) {
            o.quitOnResult = true;
        } else {
            // 未知参数：从前是静默忽略，于是 `--silnet` 这类拼写错误看起来「设了但没生效」。
            error = QStringLiteral("未知参数 %1").arg(a);
            break;
        }
    }

    if (!error.isEmpty()) {
        r.ok    = false;
        r.error = error;
        return r;
    }

    // 角色互斥：同时给两个只会在 main 末尾的 if/else 里悄悄丢掉一个。
    if (o.hostMode && o.connectMode) {
        r.ok    = false;
        r.error = QStringLiteral("--host 与 --connect 不能同时使用");
        return r;
    }

    // 动作互斥：这些动作都靠 handshakeResult 触发，同时给会互相抢着 app.exit()。
    const int actions = (o.quitOnResult ? 1 : 0) + (o.expectFrameMs > 0 ? 1 : 0)
                        + (o.measureSeconds > 0 ? 1 : 0) + (o.sendInput ? 1 : 0)
                        + (!o.sendFilePath.isEmpty() ? 1 : 0) + (o.holdTokenMs > 0 ? 1 : 0);
    if (actions > 1) {
        r.ok    = false;
        r.error = QStringLiteral("握手后的动作只能选一个（--quit-on-result / "
                                 "--expect-frame / --measure-seconds / --send-input / "
                                 "--send-file / --hold-token）");
        return r;
    }

    // 动作全都挂在 --connect 上（见 kUsage）。缺了它，有的被静默忽略
    // （--send-input / --send-file / --hold-token），有的反而把进程留在
    // 「等一个永远不会到来的握手」里（--expect-frame 只装了计时器）。
    if (actions > 0 && !o.connectMode) {
        r.ok    = false;
        r.error = QStringLiteral("握手后的动作需要配合 --connect 使用");
        return r;
    }

    r.ok = true;
    return r;
}

} // namespace lanrdp
