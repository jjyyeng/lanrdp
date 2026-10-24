#include "app/CliArgs.h"
#include "app/MainWindow.h"
#include "app/SessionStore.h"
#include "app/Theme.h"

#include "host/InputInjector.h"
#include "protocol/InputEvent.h"

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QIcon>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QTextStream>
#include <QTimer>

#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

// 用法说明见 app/CliArgs.h 里的 cliUsage()。
//
// 参数错误一律「打用法到 stderr + 退出码 2」：从前是静默丢弃（未知参数被忽略、
// `--host 9000` 漏口令时端口被当成口令、`toInt()` 失败静默变 0），用户敲错了
// 命令行却看不出任何异常，只是行为莫名其妙。
//
// ⚠️ 解析逻辑本身在 app/CliArgs.cpp —— 这样才有独立的测试目标（tests/test_cliargs.cpp）。
// 从前它全写在本文件里，而 main.cpp 带着 main() 没法链进任何测试目标，
// 于是两百行分支密集的 CLI 处理一条断言都没有。
namespace {

// 打用法到 stderr 并返回退出码 2（命令行用法错误）。
int usageError(const QString &what)
{
    QTextStream err(stderr);
    err << "参数错误：" << what << Qt::endl << lanrdp::cliUsage();
    err.flush();
    return 2;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const lanrdp::CliParseResult parsed = lanrdp::parseCliArgs(app.arguments());
    if (!parsed.ok)
        return usageError(parsed.error);

    // 沿用原来的局部名（下面几十处引用不必逐处改）。解析完成后这些值不再变化。
    const lanrdp::CliOptions &opt = parsed.options;

    const bool    quiet          = opt.quiet;
    const bool    quitOnResult   = opt.quitOnResult;
    const bool    hostMode       = opt.hostMode;
    const bool    connectMode    = opt.connectMode;
    const bool    cursorPosOnly  = opt.cursorPosOnly;
    const bool    injectLocal    = opt.injectLocal;
    const bool    sendInput      = opt.sendInput;
    const bool    useRawFrames   = opt.useRawFrames;
    const int     bitrateBps     = opt.bitrateBps;
    const quint16 hostPort       = opt.hostPort;
    const quint16 peerPort       = opt.peerPort;
    const int     expectFrameMs  = opt.expectFrameMs;
    const int     measureSeconds = opt.measureSeconds;
    const int     sendWaitMs     = opt.sendWaitMs;
    const int     holdTokenMs    = opt.holdTokenMs;
    const int     tokenWaitMs    = opt.tokenWaitMs;
    const quint16 sendNx         = opt.sendNx;
    const quint16 sendNy         = opt.sendNy;
    const quint16 localNx        = opt.localNx;
    const quint16 localNy        = opt.localNy;
    const QString password       = opt.password;
    const QString peerHost       = opt.peerHost;
    const QString recvDir        = opt.recvDir;
    const QString sendFilePath   = opt.sendFilePath;
    const QString logFilePath    = opt.logFilePath;

    // ---- 与网络无关的自检模式：不建主窗口，直接做本机能力验证 ----
    if (cursorPosOnly || injectLocal) {
        QTextStream out(stdout);

        const QRect monitor = lanrdp::virtualDesktopRect();
        out << "MONITOR " << monitor.x() << ' ' << monitor.y() << ' '
            << monitor.width() << ' ' << monitor.height() << Qt::endl;

        if (injectLocal) {
            lanrdp::InputEvent event;
            event.kind = lanrdp::InputKind::MouseMove;
            event.nx   = localNx;
            event.ny   = localNy;

            const bool  ok        = lanrdp::injectEvent(event, monitor, monitor);
            const DWORD lastError = ok ? 0 : ::GetLastError();
            out << "INJECT " << (ok ? "ok" : "failed");
            if (!ok)
                out << " lastError=" << static_cast<quint32>(lastError);
            out << Qt::endl;
        }

        const QPoint logical = QCursor::pos();
        POINT        native{};
        ::GetCursorPos(&native);
        out << "CURSOR " << logical.x() << ' ' << logical.y() << Qt::endl;
        out << "CURSOR_NATIVE " << native.x << ' ' << native.y << Qt::endl;
        out.flush();

        return 0;
    }

    // ---- 单实例保护：**刻意不做** ----
    // 曾经用 `QLockFile` 拦过「双击两次开两扇窗」，已撤掉：锁文件建不出来时程序会
    // **完全打不开**，而这功能只是省一次误操作 —— 代价明显大于收益。
    // 另外 `--host` / `--connect` 这类脚本化模式，本来也不该被「同机自测」挡在门外。
    // ⚠️ 以后若真要重做，底线是「建不出锁 ⇒ 照常启动」，绝不能反过来把启动拦掉。

    // 界面外观要在建窗口**之前**装好：窗口一创建就会按当前 palette 定标题栏与
    // 子控件的底色，之后再换样式，用户会看见闪一下（而且标题栏不一定跟着变）。
    lanrdp::theme::apply(app);

    // 窗口标题栏的图标。exe 资源段里的那个管任务栏 / 资源管理器，
    // 这里管窗口本身 —— 两处都要，少一处就会在最显眼的位置露出默认白方块。
    app.setWindowIcon(QIcon(QStringLiteral(":/lanrdp.ico")));

    // 只有真正的程序运行才接上配置落盘：测试与自动化不传这一项，因此永远不碰
    // 开发机上那份 session.ini（详见 MainWindow 构造函数的说明）。
    MainWindow window(logFilePath, lanrdp::defaultSessionFilePath());
    window.setWindowTitle(QStringLiteral("LAN RDP"));
    window.resize(1100, 760);

    const bool autoMode = hostMode || connectMode;

    if (autoMode || quitOnResult || expectFrameMs > 0) {
        QObject::connect(&window, &MainWindow::handshakeResult,
                         [&](bool ok, const QString &message) {
                             QTextStream out(stdout);
                             out << (ok ? QStringLiteral("RESULT OK") : QStringLiteral("RESULT DENY"))
                                 << ' ' << message << Qt::endl;
                             out.flush();
                             // 还在等画面 / 还要发输入 / 还要发文件时，不要因握手成功而退出
                             if (quitOnResult && expectFrameMs == 0 && !sendInput
                                 && sendFilePath.isEmpty()) {
                                 QTimer::singleShot(50, &app, [&app, ok]() { app.exit(ok ? 0 : 1); });
                             }
                         });
    }

    if (hostMode) {
        QObject::connect(&window, &MainWindow::listening, [&](quint16 port) {
            QTextStream out(stdout);
            out << "LISTENING " << port << Qt::endl;
            out.flush();
        });

        // 监听起不来（端口被占等）必须**明确失败退出**：从前只打一行日志就挂着，
        // 脚本/人工都会以为服务已经起来了，然后在「LISTENING」上干等。
        QObject::connect(&window, &MainWindow::listenFailed, [&](quint16 port, const QString &reason) {
            QTextStream err(stderr);
            err << "LISTEN FAILED " << port << ' ' << reason << Qt::endl;
            err.flush();
            // ⚠️ 必须延到事件循环里再退。`startHost()` 是在 `app.exec()` **之前**
            // 调用的，那一刻 exit() 只是记下返回码，停不掉尚未启动的事件循环 ——
            // 直接调用会变成「报了错却不退出」，`--quiet` 下静默挂死。
            QTimer::singleShot(0, &app, [&app]() { app.exit(3); });
        });

        // 自动化验证用：被控端每成功注入一次输入就打一行
        QObject::connect(&window, &MainWindow::inputInjected, [](int kind) {
            QTextStream out(stdout);
            out << "INJECTED kind=" << kind << Qt::endl;
            out.flush();
        });

        // 抓屏失败要显式报出来，否则看起来只是「没有画面」难以定位
        QObject::connect(&window, &MainWindow::captureFailed, [](const QString &error) {
            QTextStream out(stdout);
            out << "CAPTURE FAILED " << error << Qt::endl;
            out.flush();
        });

        // 收到文件（成功/失败）都打一行，供自动化验收比对 sha256
        QObject::connect(&window, &MainWindow::fileReceived,
                         [](bool ok, const QString &path, const QString &message) {
                             QTextStream out(stdout);
                             out << "FILE RECV " << (ok ? "ok" : "failed") << " path=" << path
                                 << " message=" << message << Qt::endl;
                             out.flush();
                         });
    }

    if (expectFrameMs > 0) {
        QObject::connect(&window, &MainWindow::frameReceived,
                         [&](const QSize &size, qint64 bytes) {
                             QTextStream out(stdout);
                             out << "FRAME " << size.width() << 'x' << size.height()
                                 << " bytes=" << bytes << Qt::endl;
                             out.flush();
                             QTimer::singleShot(0, &app, [&app]() { app.exit(0); });
                         });

        QTimer::singleShot(expectFrameMs, &app, [&app, expectFrameMs]() {
            QTextStream out(stdout);
            out << "FRAME TIMEOUT after " << expectFrameMs << " ms" << Qt::endl;
            out.flush();
            app.exit(2);
        });
    }

    if (measureSeconds > 0) {
        struct Counters
        {
            quint64       frames = 0;
            quint64       bytes  = 0;
            QElapsedTimer timer;
        };
        auto counters = std::make_shared<Counters>();

        QObject::connect(&window, &MainWindow::frameReceived,
                         [counters](const QSize &, qint64 bytes) {
                             if (!counters->timer.isValid())
                                 counters->timer.start();
                             ++counters->frames;
                             counters->bytes += static_cast<quint64>(bytes);
                         });

        QTimer::singleShot(measureSeconds * 1000, &app, [&app, counters, measureSeconds]() {
            const double seconds = counters->timer.isValid() ? counters->timer.elapsed() / 1000.0 : 0.0;
            const double fps     = seconds > 0.0 ? counters->frames / seconds : 0.0;
            const double mbps    = seconds > 0.0 ? (counters->bytes * 8.0 / 1e6) / seconds : 0.0;

            QTextStream out(stdout);
            out << QStringLiteral("STATS frames=%1 bytes=%2 window=%3s fps=%4 mbps=%5")
                       .arg(counters->frames)
                       .arg(counters->bytes)
                       .arg(seconds, 0, 'f', 1)
                       .arg(fps, 0, 'f', 1)
                       .arg(mbps, 0, 'f', 2)
                << Qt::endl;
            out.flush();
            app.exit(0);
        });
    }

    // ---- 控制端自动发一次输入（验证「采集 -> 封帧 -> TCP -> 被控端注入」整条链路）----
    // 先等输入令牌（连接成功后 MainWindow 已自动 requestToken），拿到才发输入。
    if (sendInput && connectMode) {
        QObject::connect(&window, &MainWindow::handshakeResult, &window,
                         [&app, &window, sendNx, sendNy, sendWaitMs, tokenWaitMs](bool ok, const QString &) {
                             QTextStream out(stdout);
                             if (!ok) {
                                 out << "SENT SKIP handshake-failed" << Qt::endl;
                                 out.flush();
                                 app.exit(1);
                                 return;
                             }

                             auto *tokenTimeout = new QTimer(&window);
                             tokenTimeout->setSingleShot(true);
                             tokenTimeout->setInterval(tokenWaitMs);

                             QObject::connect(&window, &MainWindow::tokenStateChanged, &window,
                                              [&app, &window, sendNx, sendNy, sendWaitMs, tokenTimeout](bool hasToken) {
                                                  if (!hasToken)
                                                      return;
                                                  tokenTimeout->stop();
                                                  window.sendMouseMove(sendNx, sendNy);
                                                  QTextStream inner(stdout);
                                                  inner << "SENT input-move " << sendNx << ' '
                                                        << sendNy << Qt::endl;
                                                  inner.flush();
                                                  QTimer::singleShot(sendWaitMs, &app,
                                                                     [&app]() { app.exit(0); });
                                              });

                             QObject::connect(tokenTimeout, &QTimer::timeout, &window, [&app, tokenWaitMs]() {
                                 QTextStream inner(stdout);
                                 inner << "TOKEN DENIED (no grant within " << tokenWaitMs
                                       << " ms)" << Qt::endl;
                                 inner.flush();
                                 app.exit(3);
                             });
                             tokenTimeout->start();
                         });
    }

    // ---- 控制端连接后持有输入令牌并保持 N ms（用于验收「4 控 1 仅 1 可输入」的竞争场景）----
    if (holdTokenMs > 0 && connectMode) {
        QObject::connect(&window, &MainWindow::handshakeResult, &window,
                         [&app, &window, holdTokenMs, tokenWaitMs](bool ok, const QString &) {
                             QTextStream out(stdout);
                             if (!ok) {
                                 out << "TOKEN SKIP handshake-failed" << Qt::endl;
                                 out.flush();
                                 app.exit(1);
                                 return;
                             }

                             auto *tokenTimeout = new QTimer(&window);
                             tokenTimeout->setSingleShot(true);
                             tokenTimeout->setInterval(tokenWaitMs);

                             QObject::connect(&window, &MainWindow::tokenStateChanged, &window,
                                              [&app, holdTokenMs, tokenTimeout](bool hasToken) {
                                                  if (!hasToken)
                                                      return;
                                                  tokenTimeout->stop();
                                                  QTextStream inner(stdout);
                                                  inner << "TOKEN HELD" << Qt::endl;
                                                  inner.flush();
                                                  QTimer::singleShot(holdTokenMs, &app,
                                                                     [&app]() { app.exit(0); });
                                              });

                             QObject::connect(tokenTimeout, &QTimer::timeout, &window, [&app]() {
                                 QTextStream inner(stdout);
                                 inner << "TOKEN DENIED" << Qt::endl;
                                 inner.flush();
                                 app.exit(3);
                             });
                             tokenTimeout->start();
                         });
    }

    // ---- 控制端发一个文件（验证「读文件 -> 分块 -> TCP -> 对端落盘 + sha256 校验」整条链路）----
    if (!sendFilePath.isEmpty() && connectMode) {
        QObject::connect(&window, &MainWindow::handshakeResult, &window,
                         [&app, &window, sendFilePath](bool ok, const QString &) {
                             QTextStream out(stdout);
                             if (!ok) {
                                 out << "FILE SENT skip handshake-failed" << Qt::endl;
                                 out.flush();
                                 app.exit(1);
                                 return;
                             }
                             QTimer::singleShot(200, &window, [&app, &window, sendFilePath]() {
                                 if (!window.sendFile(sendFilePath)) {
                                     QTextStream inner(stdout);
                                     inner << "FILE SENT not-started (文件不可读或连接已断开)"
                                           << Qt::endl;
                                     inner.flush();
                                     app.exit(1);
                                 }
                             });
                         });

        QObject::connect(&window, &MainWindow::fileSent, &window,
                         [&app, &window](bool ok, const QString &path, const QString &message) {
                             QTextStream out(stdout);
                             out << "FILE SENT " << (ok ? "ok" : "failed") << " path=" << path
                                 << " message=" << message << Qt::endl;
                             out.flush();

                             // 送完**不能立刻就退**：`fileSent` 只表示「帧已排进 Qt 的
                             // 用户态写缓冲」—— 实测一个 256 KB 的文件在这一刻缓冲里
                             // 还有 262309 字节没交给内核，所以「等一个固定毫秒数再退出」
                             // 必然不可靠（分界点随链路快慢漂移；从前那条 500 ms 就是
                             // 这个毛病，384 KB 起必丢）。
                             // 收尾交给 halfCloseClientWhenFlushed()：等缓冲排空后
                             // **半关**写方向（FIN，不是会回 RST 的 close），对端读到
                             // EOF 会关掉它那一侧；本端连接真正结束时 `clientClosed`
                             // 才到，那时退出不会再打断在途数据。
                             const int exitCode = ok ? 0 : 1;
                             QObject::connect(&window, &MainWindow::clientClosed, &app,
                                              [&app, exitCode]() { app.exit(exitCode); });
                             // 兜底：对端彻底没反应时别把命令行挂死（正常路径走不到）。
                             QTimer::singleShot(60000, &app, [&app, exitCode]() { app.exit(exitCode); });
                             window.halfCloseClientWhenFlushed();
                         });
    }

    if (!autoMode || !quiet)
        window.show();

    if (!recvDir.isEmpty())
        window.setReceiveDir(recvDir);

    if (hostMode) {
        // 默认 H.264；--raw 强制裸帧用于对照测试
        window.setHostCodec(!useRawFrames, bitrateBps);
        window.startHost(hostPort, password);
    } else if (connectMode) {
        window.startConnect(peerHost, peerPort, password);
    }

    return app.exec();
}
