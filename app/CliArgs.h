#pragma once

#include <QString>
#include <QStringList>

#include <QtGlobal>

namespace lanrdp {

// 启动参数解析结果。
//
// 抽出来的唯一理由是**可测**：这些分支（未知参数 / 缺取值 / 数字非法 / 动作互斥）
// 从前全写在 `main()` 里，而 `main.cpp` 带着 `main()` 没法链进测试目标 ——
// 于是两百行分支密集的 CLI 处理一条断言都没有。
// 解析只做「读字符串 → 校验 → 填结构」，不碰网络也不建窗口，可以单独跑。
struct CliOptions
{
    bool quiet         = false;
    bool quitOnResult  = false;
    bool hostMode      = false;
    bool connectMode   = false;
    bool cursorPosOnly = false;
    bool injectLocal   = false;
    bool sendInput     = false;
    bool useRawFrames  = false;

    int     bitrateBps     = 8 * 1000 * 1000;
    quint16 hostPort       = 9000;
    quint16 peerPort       = 9000;
    int     expectFrameMs  = 0;
    int     measureSeconds = 0;
    int     sendWaitMs     = 400;
    int     holdTokenMs    = 0;     // --hold-token：连接后持有输入令牌并保持 N ms
    int     tokenWaitMs    = 2000;  // --token-wait：等待 TokenGrant 的超时（默认 2 s）
    quint16 sendNx         = 0;
    quint16 sendNy         = 0;
    quint16 localNx        = 0;
    quint16 localNy        = 0;

    QString password;
    QString peerHost;
    QString recvDir;
    QString sendFilePath;
    QString logFilePath;
};

// 用法文本（参数错误时随错误原因一起打到 stderr）。
const char *cliUsage();

struct CliParseResult
{
    bool       ok = false;
    CliOptions options;
    QString    error;   // ok == false 时给用户的说明
};

// 解析 `args`（**含** argv[0]，与 QCoreApplication::arguments() 同口径）。
// ok == false 时调用方应「打 usage 到 stderr + 退出码 2」。
CliParseResult parseCliArgs(const QStringList &args);

} // namespace lanrdp
