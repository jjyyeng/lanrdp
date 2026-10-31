// ⚠️ windows.h 必须排在 Qt 头之前：它定义了一堆短名宏（min/max/interface…），
// 排在 Qt 之后会把 Qt 头里的标识符替换掉。NOMINMAX + WIN32_LEAN_AND_MEAN 是
// Qt 工程里的标准搭配（前者禁掉 min/max 宏，后者砍掉 winsock1 等重头文件）。
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
// 半关（shutdown 的 SD_SEND）要用 winsock2；WIN32_LEAN_AND_MEAN 已经挡掉
// windows.h 里那份老的 winsock1，所以这里直接包含不会撞。
#include <winsock2.h>
#include <windows.h>

#include "app/MainWindow.h"

#include "app/Dialogs.h"
#include "app/Glyph.h"
#include "app/Theme.h"
#include "host/HostController.h"
#include "net/ClientConnection.h"
#include "net/ClipboardSync.h"
#include "net/FileReceiver.h"
#include "net/HostServer.h"
#include "net/Session.h"
#include "protocol/AudioChunk.h"
#include "protocol/Frame.h"
#include "protocol/InputEvent.h"
#include "viewer/AudioPlayer.h"
#include "viewer/EmptyState.h"
#include "viewer/RemoteView.h"
#include "viewer/ScreenOverlayBar.h"
#include "viewer/ScreenRenderer.h"
#include "viewer/ViewGrid.h"

#include <QAction>
#include <QActionGroup>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QProgressBar>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStatusBar>
#include <QStringList>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVector>
#include <QWidget>

#include <memory>
#include <utility>

using namespace lanrdp;

namespace {
constexpr quint16 kDefaultPort = 9000;

// 控制端心跳（3 s）/ 死链判定（10 s）/ 重连退避上限（30 s）
// 这三条常量已随心跳与重连逻辑一起移入 net/ClientConnection.cpp（它们只服务那条连接）。

// 一控多的格数上限（最多 4 台）。
// 4 台是「按最小格子取显示预算也不亏」的一档：4K 屏上做 2×2，单格面积正好是
// 源的 1/4 —— 再往上加就进死区了（详见 ClientConnection::setDisplayBudget 的说明）。
constexpr int kMaxViews = 4;

QString humanBytes(quint64 bytes)
{
    if (bytes >= 1024ull * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 2);
    if (bytes >= 1024ull * 1024)
        return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
    if (bytes >= 1024ull)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

// 「ip:port」形态的对端标识与「毫秒转人话」都随被控端逻辑搬进了
// host/HostController.cpp —— 它们只在那边的日志里用得上。

// ---- 传输进度条辅助（发送端 / 接收端共用）----
// 进度条常态隐藏，传输开始才出现，结束即复位并隐藏，不改变既有布局观感。

void beginProgress(QProgressBar *bar, const QString &verb, const QString &name, quint64 total)
{
    if (!bar)
        return;

    // total 为 0（未知大小）时用 (0,0) 显示不确定态，而不是空进度条
    bar->setRange(0, total > 0 ? int(qMin<quint64>(total, quint64(INT_MAX))) : 0);
    bar->setValue(0);
    bar->setFormat(QStringLiteral("%1 %2  %p%").arg(verb, name));
    bar->show();
}

void stepProgress(QProgressBar *bar, quint64 done, quint64 total)
{
    if (!bar || total == 0)
        return;

    bar->setValue(int(qMin<quint64>(done, quint64(bar->maximum()))));

    // 传输结束那一拍可能被 200ms 节流吃掉，这里保证能走到 100%
    if (done >= total)
        bar->setValue(bar->maximum());
}

void endProgress(QProgressBar *bar)
{
    if (!bar)
        return;

    bar->setValue(0);
    bar->hide();
}

} // namespace

MainWindow::MainWindow(const QString &logFilePath, const QString &sessionFilePath,
                       QWidget *parent)
    : QMainWindow(parent)
    , sessionStore_(sessionFilePath)
{
    // 日志落盘到 exe 同目录（即 dist/lanrdp/lanrdp.log）。
    //
    // ⚠️ 用 applicationDirPath() 而不是相对路径：程序被 Explorer 双击时，
    // 进程工作目录**不保证**是 exe 所在目录，相对路径会让日志散落到意外位置。
    // ⚠️ 目录不可写时 LogPort 会静默降级为「只进内存缓冲」，不弹窗、不报错 ——
    // 日志系统不该拖垮主程序。启动第一行日志会写明实际路径，事后可据此判断。
    // 入参非空则改用调用方给的路径：测试与 CLI 自测靠它隔离（见 MainWindow.h 的说明）。
    const QString logPath = logFilePath.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath())
              .absoluteFilePath(QStringLiteral("lanrdp.log"))
        : logFilePath;
    logPort_ = std::make_unique<lanrdp::LogPort>(logPath);

    // 上次用过的会话参数（端口 / 口令 / 地址 / 接收目录 / 编码）。
    //
    // 口令在 ini 里是 DPAPI 密文，解不开（换了 Windows 用户或换了机器）就当作没存过，
    // 这里拿到的就是空串 —— 对话框里仍是空的，用户重输一次即可。**不要**把解不开
    // 当成错误弹出来：那是"配置该重填"的正常情形，不是故障。
    //
    // 必须在 buildUi() 之前落定：下面建宿主时直接拿这几个成员喂 setReceiveDir/setCodec，
    // 晚一步就会出现「界面已按旧参数起来、之后才被改掉」的中间态。
    if (sessionStore_.isEnabled()) {
        const lanrdp::HostSession host = sessionStore_.loadHost();
        hostPort_     = host.port;
        hostPassword_ = host.password;
        hostRecvDir_  = host.receiveDir;
        hostUseH264_  = host.useH264;

        const lanrdp::PeerSession peer = sessionStore_.loadPeer();
        peerHost_     = peer.host;
        peerPort_     = peer.port;
        peerPassword_ = peer.password;
    }

    // 控制端连接**不再在这里建** —— 它现在属于某一格（RemoteView）。
    // 第 0 格（本机预览格）在 buildUi() 里建好，且**没有连接**：只有真的要连一台
    // 远端时才给它挂上一条。这样「没连过」就是「没有连接对象」，不必再维护一个
    // 「连接对象存在但其实空闲」的中间态。
    buildUi();          // 建视窗 + 接线（含每格的条上控件与网格选中）
    setupClipboard();   // 剪贴板同步（仅文本）

    // ---- 音频播放（对方的声音）----
    // 单实例：只有选中格那一格出声。**构造时不碰任何设备** —— 第一次 `begin()`
    // 才起线程，所以几十个 UI 用例构造本窗口不会去开声卡。
    audioPlayer_ = new lanrdp::AudioPlayer(this);
    connect(audioPlayer_, &lanrdp::AudioPlayer::failed, this, [this](const QString &reason) {
        log(QStringLiteral("听不到对方的声音（画面不受影响）：%1").arg(reason));
        setStatusText(QStringLiteral("本机音频输出不可用，听不到对方的声音"));
    });

    // 音频体检：10 s 一次，**只在计数变化时写一行日志**。
    // 声音「卡一下」的原因分几种（网络抖 / 对端供不上 / 本机设备欠载 / 收得太慢丢块），
    // 界面上看不出来，全靠这三个数落进日志 —— 所以它不进状态栏（不占像素），只进日志。
    auto *audioHealth = new QTimer(this);
    audioHealth->setInterval(10000);
    connect(audioHealth, &QTimer::timeout, this, &MainWindow::reportAudioHealth);
    audioHealth->start();

    // ---- 被控端宿主：一条工作线程 + 一个对象 ----
    // 监听 / 会话池 / 抓屏编码 / 输入注入 / 文件落盘 / 令牌门控全在它里面，而且**跑在
    // 自己的工作线程上**（`host_->moveToThread(hostThread_)`）。本窗口只把它的信号呈现成界面。
    // 放在 buildUi() 之后建：下面这些槽会摸 UI，不能让信号在界面成型前到达。
    hostThread_ = new QThread(this);
    // ⚠️ 宿主**必须无 parent** —— 有 parent 的对象不能 `moveToThread`。
    host_ = new lanrdp::HostController(logPort_.get());

    // 启动参数必须在 `moveToThread` **之前**喂进去：此刻它还在本线程，直接写成员没有
    // 并发问题；移进工作线程之后，这些写访问器会断言「必须在自己的线程里调用」
    //（见 `HostController` 的线程纪律）。线程一起来就该按这套配置跑。
    host_->setReceiveDir(hostRecvDir_);
    host_->setCodec(hostUseH264_, hostBitrate_);

    host_->moveToThread(hostThread_);
    // 对象归属工作线程 ⇒ 只能在**那个线程**里销毁：Qt 官方范式。
    //（`QThreadPrivate::finish()` 在 emit `finished` 之后紧接着派发 DeferredDelete。
    //  用 `delete host_` 在主线程删，会连带在错误线程析构它的 QTimer 子对象。）
    connect(hostThread_, &QThread::finished, host_, &QObject::deleteLater);

    connect(host_, &lanrdp::HostController::listening, this, &MainWindow::listening);
    connect(host_, &lanrdp::HostController::listenFailed, this, &MainWindow::listenFailed);
    connect(host_, &lanrdp::HostController::logLine, this, &MainWindow::log);
    connect(host_, &lanrdp::HostController::statusText, this, &MainWindow::setStatusText);

    // 状态 / 列表 / 统计一律走**快照**：GUI 一次都不去读工作线程的成员。
    connect(host_, &lanrdp::HostController::uiStateChanged, this,
            [this](const lanrdp::HostController::UiState &state) {
                hostState_ = state;
                refreshClientList();
            });

    // 帧自身上报的尺寸是状态栏分辨率口径（被控端的画面只推给对端，本机不预览）。
    connect(host_, &lanrdp::HostController::frameSizeChanged, this,
            [this](const QSize &size) { lastFrameSize_ = size; });

    // ---- 接收文件：宿主只报事件与数字，文案 / 进度条在这里做 ----
    connect(host_, &lanrdp::HostController::fileStarted, this,
            [this](const QString &name, quint64 total) {
                log(QStringLiteral("正在接收文件：%1（%2）").arg(name, humanBytes(total)));
                setStatusText(QStringLiteral("接收中：%1").arg(name));
                beginProgress(recvProgress_, QStringLiteral("接收中"), name, total);
            });
    // 进度按时间节流，避免大文件时刷屏。
    // 多个控制端同时传时，状态栏显示**最近有进展的那一路**（谁更新就显示谁）。
    connect(host_, &lanrdp::HostController::fileProgress, this,
            [this, lastTick = std::make_shared<QElapsedTimer>()](quint64 received, quint64 total) {
                if (total == 0)
                    return;
                if (received < total && lastTick->isValid() && lastTick->elapsed() < 200)
                    return;
                lastTick->restart();
                stepProgress(recvProgress_, received, total);
                setStatusText(QStringLiteral("接收中 %1 / %2 (%3%)")
                                  .arg(humanBytes(received))
                                  .arg(humanBytes(total))
                                  .arg(int(received * 100 / total)));
            });
    connect(host_, &lanrdp::HostController::fileReceived, this,
            [this](bool ok, const QString &path, const QString &message) {
                endProgress(recvProgress_);
                if (ok) {
                    log(QStringLiteral("文件已接收并校验通过：%1").arg(path));
                    setStatusText(QStringLiteral("文件接收完成"));
                } else {
                    log(QStringLiteral("文件接收失败：%1").arg(message));
                    setStatusText(QStringLiteral("文件接收失败"));
                }
                emit fileReceived(ok, path, message);
            });

    connect(host_, &lanrdp::HostController::inputInjected, this, &MainWindow::inputInjected);
    connect(host_, &lanrdp::HostController::captureFailed, this, &MainWindow::captureFailed);

    // 剪贴板：宿主只做「发送者是否持令牌」的判定，写系统剪贴板必须留在 GUI 线程
    //（`QClipboard` 只能在 GUI 线程碰）。
    connect(host_, &lanrdp::HostController::clipboardFrameReceived, this,
            [this](const lanrdp::Frame &frame) {
                if (clipboardSync_)
                    clipboardSync_->handleFrame(frame);
            });

    // 令牌状态变化 -> 刷一次界面。接在**构造期**（这里），不是 `wireView()` ——
    // 那个函数每加一格调一次，而这条信号与格子无关，跟着格子接就会挂上 N 份
    // 相同的 lambda，每次令牌变化白跑 N 遍全量刷新。
    connect(this, &MainWindow::tokenStateChanged, this, [this](bool) {
        updateTokenStatus();
    });

    // 接线齐了才让宿主线程跑起来 —— 否则第一批信号可能落到还没接好的槽上。
    // 宿主内部的定时器/套接字都由 `start()` 在工作线程里创建（见 HostController::ensureTimers）。
    hostThread_->start();

    log(QStringLiteral("日志文件：%1").arg(logPort_->logFilePath()));
    if (!logPort_->fileWritable()) {
        log(QStringLiteral("⚠️ 日志文件不可写，本次运行的日志**不会落盘**（仅内存）"));
        // 「日志写不进去」这件事不能只写进那份写不进去的日志 —— 必须走**看得见**的通道。
        setStatusText(QStringLiteral("日志写不进文件（不影响使用）：%1")
                          .arg(logPort_->logFilePath()));
    }
}

// 控制端连接的三个状态查询 —— 一律问**选中格**那条连接。
// 定义放 .cpp（而非头文件内联）是因为 MainWindow.h 里 ClientConnection 只有前向声明，
// 内联会报 incomplete type。
bool MainWindow::clientReady() const
{
    auto *c = activeConnection();
    return c && c->ready();
}

bool MainWindow::canRequestToken() const
{
    auto *c = activeConnection();
    return c && c->canRequestToken();
}

bool MainWindow::hasToken() const
{
    auto *c = activeConnection();
    return c && c->hasToken();
}

MainWindow::~MainWindow()
{
    // ⚠️ 停机顺序**不能动**：
    //   ① `stopHost()` 必须在线程**还活着**时调 —— 它走阻塞投递，线程一停就永久死锁。
    //   ② `wait()` 必须是**无限**的：带超时的话，超时后只能「在仍在跑的线程上删对象」，
    //      那比卡住更糟（项目也早已禁掉 `terminate()`）。
    //   ③ 停机完成之前不能动 `logPort_`：宿主线程与看门狗都会往里写日志。

    // 「连着呢就直接关窗口」是很常见的操作，这条退出路径必须先掐断网络信号：
    // QTcpSocket 的析构会**同步**发出 disconnected / errorOccurred，而 QWidget 的析构
    // 这时已经把子控件删掉了；槽函数再调用 log() / refreshClientList() 就会踩到
    // 悬空指针，直接崩在退出路径上。
    const auto sockets = findChildren<QTcpSocket *>();
    for (QTcpSocket *sock : sockets)
        sock->disconnect();

    // ① 停被控端业务（宿主此刻仍在工作线程上跑着，阻塞投递才完得成）。
    stopHost();

    // 音频播放线程：它只碰本机声卡，与宿主线程无关，但必须在成员析构前停干净
    //（`~AudioPlayer` 也会兜一次；这里显式停是为了「窗口正在拆」的那段空窗不再收帧）。
    if (audioPlayer_)
        audioPlayer_->stopAndWait();

    if (hostThread_) {
        // ② 掐掉「宿主 → 本窗口」的回连：`~HostController` 在**工作线程里**析构时会调
        //    `stop()` 并 emit 日志 / 状态，而本窗口已经在拆了，不该再收这些。
        if (host_)
            disconnect(host_, nullptr, this, nullptr);
        // ③ 让事件循环退出，并**等线程真的结束**。
        hostThread_->quit();
        hostThread_->wait();
        // 宿主对象由 `finished → deleteLater` 在工作线程里销毁，这里只把指针放掉。
        host_ = nullptr;
    }

    // 每格那条连接的内核清理（停定时器 / 拆会话 / 释放令牌）。
    // 连接归 RemoteView（unique_ptr）持有，这里显式 teardown 是为了让
    // 「先掐信号、再拆」的顺序可控；之后 unique_ptr 析构时还会再走一遍（幂等）。
    for (lanrdp::RemoteView *v : std::as_const(views_)) {
        if (v && v->connection())
            v->connection()->teardown();
    }
}

void MainWindow::buildUi()
{
    // ---- 中央：多视窗网格 ----
    //
    // 中央区是承载 1..4 个视窗的网格宿主。`setCentralWidget()` 只能有一个，
    // 而一控多要求中央区同时放多路画面 —— 所以「画面」从中央部件降为**格子**
    //（`RemoteView`）。第 0 格在启动时就存在（本机预览格），连上远端才加格。
    grid_ = new lanrdp::ViewGrid(this);
    setCentralWidget(grid_);

    // 每格自带一条顶部悬浮条（**不进布局** —— 塞进布局会挤占画面高度，
    // 而"悬浮"的定义就是浮在上面）。它作为该格画面控件的子控件，自己跟随尺寸定位。
    buildViews();

    // 网格选中变化 -> 更新每格的选中外观（条上的强调线）。
    connect(grid_, &lanrdp::ViewGrid::selectedChanged,
            this, &MainWindow::onGridSelectionChanged);

    // 窗口级 QSS 已经不在这里了：条上按钮的「主操作加粗 / 危险操作标红」随
    // 其余条内样式一起进了 app/Theme.cpp。分两处写必然有一处忘了跟着深色化 ——
    // 危险色从前是 #8a1f11，压在深色条上几乎看不见。

    statusLabel_ = new QLabel(QStringLiteral("就绪"), this);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusBar()->addWidget(statusLabel_);

    // 两条传输进度条落在**状态栏**。
    // 放悬浮条上会让条宽随文件名 / 百分比不停跳动，状态栏本来就在显示类似信息。
    // 常态隐藏（隐藏时在状态栏里不占宽度），传输时出现，结束复位。
    recvProgress_ = new QProgressBar(this);
    recvProgress_->setObjectName(QStringLiteral("recvProgress"));
    recvProgress_->setMaximumWidth(180);
    recvProgress_->setToolTip(QStringLiteral(
        "正在接收对方发送的文件。\n"
        "完成后将自动校验文件完整性。"));
    recvProgress_->setVisible(false);
    statusBar()->addPermanentWidget(recvProgress_);

    sendProgress_ = new QProgressBar(this);
    sendProgress_->setObjectName(QStringLiteral("sendProgress"));
    sendProgress_->setMaximumWidth(180);
    sendProgress_->setToolTip(QStringLiteral(
        "正在向对方发送文件。\n"
        "也可以将文件拖入画面发送。"));
    sendProgress_->setVisible(false);
    statusBar()->addPermanentWidget(sendProgress_);

    // 【被控】菜单（排在【视图】之前 —— 角色动作比视图偏好更上层）
    buildHostMenu();
    buildViewMenu();   // 悬浮条显隐入口

    statsTimer_ = new QTimer(this);
    statsTimer_->setInterval(1000);
    connect(statsTimer_, &QTimer::timeout, this, &MainWindow::updateStats);
    statsTimer_->start();

    // 空闲让出的兜底判定（5 s）与输入看门狗（200 ms 打点 + 独立线程）
    // 都已随被控端逻辑搬进 HostController —— 它们服务的是「令牌持有者」与
    // 「读 socket + 注入」这条链路，本来就该跟被控端同生共死。

    // 控制端的心跳（3 s）与重连退避（singleShot）定时器已随状态一起
    // 移入 lanrdp::ClientConnection —— 它们天然是「每条连接一份」的东西。

    // 初始空态文案与条上控件组由 refreshViewUi() 统一落定（见 updateConnectionUi 末尾）
    updateConnectionUi();  // 初始按钮状态（断开 / 发送文件在未连接时不可用）+ 模式分组显隐
    updateHostTokenUi();   // 初始图标状态（未锁定 = 闭锁图标）
}

// ---------------------------------------------------------------------------
// 视窗（一控多的「一格」）
//
// 每格是一个 `RemoteView`：画面 + 悬浮条 + 条上控件 + **该格的那条连接**。
// 本窗口在这里只做三件事：建格 / 加格 + 重排、把每格的零件接到本窗口、
// 以及把「选中哪一格」这件事刷到外观上。
//
// ⚠️ 接线是**按格**接（回调里捕获那一格的指针），不是「按选中接」：
// · 画面的键鼠事件自带来源（用户是在第 2 格的画面上按的），把它发给「当前选中格」
// 是错的 —— 事件本身已经指明是哪一路；
// · 条上的动作按钮（【断开】【发送文件…】）同理，各自带格。
// 真正**跟随选中**的只有：程序化入口（`sendMouseMove` 等自测 API）、
// 状态栏统计、剪贴板同步目标 —— 这些是「用户当下在看/在操作哪一路」的表述。
//
// ⚠️ 条上控件的 objectName 沿用旧名，见 viewer/RemoteView.h。
// ---------------------------------------------------------------------------

// 第 0 格：启动时就建好。它是启动空态格（没有连接对象，内显「连接对方 / 让对方连进来」
// 两个入口），控制端每连一台再加格。格子没有主次 —— 标题与显隐由 refreshViewUi
// 按各格自己的连接状态统一刷。
void MainWindow::buildViews()
{
    addView();
}

int MainWindow::maxViews()
{
    return kMaxViews;
}

lanrdp::RemoteView *MainWindow::viewAt(int index) const
{
    if (index < 0 || index >= views_.size())
        return nullptr;
    return views_.at(index);
}

lanrdp::RemoteView *MainWindow::selectedView() const
{
    if (!grid_)
        return views_.isEmpty() ? nullptr : views_.first();
    return viewAt(grid_->selectedIndex());
}

lanrdp::ScreenOverlayBar *MainWindow::overlayBar() const
{
    auto *v = selectedView();
    return v ? v->bar() : nullptr;
}

lanrdp::ScreenRenderer *MainWindow::screenRenderer() const
{
    auto *v = selectedView();
    return v ? v->renderer() : nullptr;
}

lanrdp::ClientConnection *MainWindow::connectionOf(lanrdp::RemoteView *view) const
{
    return view ? view->connection() : nullptr;
}

lanrdp::ClientConnection *MainWindow::activeConnection() const
{
    return connectionOf(selectedView());
}

lanrdp::ScreenRenderer *MainWindow::activeRenderer() const
{
    auto *v = selectedView();
    return v ? v->renderer() : nullptr;
}

// 「这一格能不能拿来连下一台」。
// · 没有连接对象（还没连过）→ 能；
// · 有连接但已经死透（既没连上、也没排重连）→ 能，等价于「换一台重连」；
// · 活着 / 正在等重连 → 不能，那是「再开一格」的情形。
// 这条规则让「点【新连接…】换一台」与「点【新连接…】再开一台」自动落到同一段代码上。
bool MainWindow::viewIsReusable(const lanrdp::RemoteView *view)
{
    if (!view)
        return false;
    const lanrdp::ClientConnection *c = view->connection();
    if (!c)
        return true;
    return !c->connected() && !c->reconnectPending();
}

lanrdp::RemoteView *MainWindow::addView()
{
    auto *v = new lanrdp::RemoteView(grid_);
    views_.append(v);

    wireView(v);   // 条上控件 -> 本窗口（每格只接一次）

    // 新格继承「悬浮条是否收起」这个全局偏好（否则收起之后再开新连接，
    // 条会突然又冒出来，像是设置没生效）。
    v->bar()->setVisible(!barsHidden_);

    // 加新格之前**先退出占满** —— 占满态下只有那一格显示，新格一出生
    // 就是隐藏的；用户刚点了【新连接…】却看不到新格子，只会怀疑自己没连上。
    // （ViewGrid 只会归一化「越界的」占满索引，格子变多时它仍然合法，所以得显式退。）
    if (grid_->isMaximized())
        grid_->setMaximizedIndex(-1);

    // 重排。ViewGrid 自己会「先 removeWidget 全部再加」并归一化越界的选中 / 占满索引。
    QVector<QWidget *> ws;
    ws.reserve(views_.size());
    for (lanrdp::RemoteView *x : std::as_const(views_))
        ws.append(x);
    grid_->setViews(ws);

    // 新加的格默认成为焦点：用户刚点了【新连接…】，接下来要操作的就是它。
    grid_->setSelectedIndex(views_.size() - 1);
    onGridSelectionChanged(grid_->selectedIndex());

    if (views_.size() > 1)
        log(QStringLiteral("视窗增至 %1 格").arg(views_.size()));

    // 格数变了 ⇒ 每格的尺寸与【占满】入口的可用性都变了。
    syncMaximizeState();
    scheduleDisplayBudget();
    return v;
}

// 把一格条上的控件接到本窗口。每格接一次；回调里**必须**捕获 `v`。
void MainWindow::wireView(lanrdp::RemoteView *v)
{
    // 空态层上的两个入口。它们和【被控】菜单走同一条路 ——
    // 只是摆在了用户第一眼能看到的地方，而不是等他自己去菜单里找。
    connect(v->emptyState(), &lanrdp::EmptyState::connectRequested,
            this, &MainWindow::openConnectDialog);
    connect(v->emptyState(), &lanrdp::EmptyState::hostRequested, this, [this] {
        // 与【被控】菜单同一语义：未监听 = 弹对话框；已监听 = 停止监听。
        if (isHosting())
            stopHost();
        else
            openHostConfigDialog();
    });

    // ---- 控制端：一格的动作只作用在这一格那条连接上 ----
    // 【连接另一台】= 只加一格空视窗（内显空态两入口），不弹对话框 ——
    // 「连出去还是让对方连进来」由用户新格的空态里再选。
    connect(v->connectButton(), &QToolButton::clicked, this, &MainWindow::addEmptyView);
    // 「已连 N 个」是被控端的全局状态按钮（条上最左），点开弹出连接管理菜单。
    connect(v->clientListButton(), &QToolButton::clicked, this,
            [this, v] { onClientListMenu(v->clientListButton()); });
    connect(v->disconnectButton(), &QToolButton::clicked, this,
            [this, v] { disconnectView(v); });
    connect(v->sendFileButton(), &QToolButton::clicked, this,
            [this, v] { sendFileFrom(v); });
    connect(v->releaseTokenButton(), &QToolButton::clicked, this, [this, v] {
        if (auto *c = connectionOf(v))
            c->releaseToken();
    });
    // 【屏幕】：只在被控端有多块屏时可见（单屏用户界面零变化）。
    connect(v->screenButton(), &QToolButton::clicked, this, [this, v] {
        QMenu *menu = createScreenMenu(v);
        if (!menu)
            return;
        menu->exec(v->screenButton()->mapToGlobal(
            QPoint(0, v->screenButton()->height())));
        menu->deleteLater();
    });
    // 断线格的两个出口（按格 —— 断的是哪一台，重连/关闭就是哪一台）。
    connect(v->retryButton(), &QToolButton::clicked, this, [this, v] { retryView(v); });
    connect(v->closeCellButton(), &QToolButton::clicked, this, [this, v] { closeView(v); });

    // 【占满 / 还原】——按格动作：点第 2 格的它就占满第 2 格。
    connect(v->maximizeButton(), &QToolButton::clicked, this,
            [this, v] { toggleMaximize(v); });
    // 双击条标题 = 同一个动作（条已经是本地手势区，双击不会漏给远端）。
    // 断线格不接：那种格子该做的是【重连】/【关闭】，把它占满没有意义。
    connect(v->bar(), &lanrdp::ScreenOverlayBar::doubleClicked, this, [this, v] {
        if (!viewIsDisconnected(v))
            toggleMaximize(v);
    });
    // 占满态下的 Esc（RemoteView 已经把那颗键**吃掉**了，不会漏给远端）。
    connect(v, &lanrdp::RemoteView::restoreRequested, this,
            [this, v] { restoreMaximize(v); });

    // 点这一格 = 选中这一格（RemoteView 旁听画面 / 条的点击后上报，不消费事件）。
    connect(v, &lanrdp::RemoteView::activated, this, [this, v] {
        if (grid_)
            grid_->setSelectedIndex(views_.indexOf(v));
    });
}

// ---------------------------------------------------------------------------
// 选中（「点哪一格就操作哪一格」）
//
// ⚠️ 选中**不改变任何连接** —— 它只决定「跟随选中的那些操作发给谁」，
// 以及条上那条强调线画在哪一格。
// ⚠️ 之所以把选中外观画在**条**上而不是整格描边：描边要占掉 2px 画面高度，
// 尺寸一变显示预算就跟着抖（解码器可能因此重建）。条本来就是每格的「抬头」，
// 焦点画在那里既明显又不碰几何。
// ---------------------------------------------------------------------------
void MainWindow::onGridSelectionChanged(int index)
{
    Q_UNUSED(index);

    const int sel = grid_ ? grid_->selectedIndex() : 0;
    for (int i = 0; i < views_.size(); ++i) {
        lanrdp::RemoteView *v = views_.at(i);
        if (!v)
            continue;
        v->setSelected(i == sel);
    }

    // 条标题里含「对端是谁」这类跟随状态的信息，选中一变就重刷。
    refreshViewUi();
    updateTokenStatus();

    // 声音跟着选中格走：切格要**立刻**换声，并把上一格的尾巴清掉。
    applyAudioRouting();
}

// 这一格此刻该不该出声：开关开着，且它就是当前选中格。
//
// 一控多时四路声音叠在一起没法听（也没有「谁更重要」的客观标准），所以只播一格。
// 「按选中」而不是「按全部混音」还省掉了求和 + 限幅这一整套。
bool MainWindow::isAudioSource(lanrdp::RemoteView *view) const
{
    if (!audioEnabled_ || !view || !grid_)
        return false;
    return view == views_.value(grid_->selectedIndex(), nullptr);
}

void MainWindow::applyAudioRouting()
{
    if (!audioPlayer_)
        return;

    // 无论开还是关，先把缓冲清空：否则「关掉声音」后还会把手上那 200 ms 播完，
    // 而切格时会先听到上一台电脑的尾巴。
    audioPlayer_->reset();

    if (!audioEnabled_)
        return;

    auto *c = activeConnection();
    // 格式用的是**这条连接缓存的那一份**（不是等到下一帧配置）：
    // 错过开局那一帧配置的话，就得等对端换默认设备才会再有配置帧。
    if (c && c->hasAudioFormat())
        audioPlayer_->begin(c->audioFormat());
}

// 音频体检：正常就**一行都不写**，只在计数有变化时留一条证据。
//
// 判读口径（这是「卡一下」唯一能事后区分原因的地方）：
// · 只有「断流」涨 → 网络抖动 / 对端供不上，音频链路本身没问题；
// · 只有「丢弃」涨 → 本机处理不过来，抖动缓冲被上限裁了（延迟没炸，但声音有断层）；
// · 两者都涨 + 队列深度接近 0 → 本机设备在被别的东西抢（或蓝牙链路本身在抖）。
void MainWindow::reportAudioHealth()
{
    if (!audioPlayer_)
        return;

    const quint64   starved = audioPlayer_->starvedCount();
    const qsizetype dropped = audioPlayer_->droppedBytes();

    // 没在播、或缓冲刚被 reconfigure 重置（计数变小）⇒ 只拉平基线，不报。
    if (!audioPlayer_->isActive() || starved < audioStarvedSeen_ || dropped < audioDroppedSeen_)
    {
        audioStarvedSeen_ = starved;
        audioDroppedSeen_ = dropped;
        return;
    }

    const quint64   starvedDelta = starved - audioStarvedSeen_;
    const qsizetype droppedDelta = dropped - audioDroppedSeen_;
    if (starvedDelta == 0 && droppedDelta == 0)
        return;

    audioStarvedSeen_ = starved;
    audioDroppedSeen_ = dropped;

    QString line = QStringLiteral("音频体检：设备队列 %1 ms").arg(audioPlayer_->queuedMs());
    if (starvedDelta > 0)
        line += QStringLiteral("，断流 %1 次（网络抖动或对端供不上）").arg(starvedDelta);
    if (droppedDelta > 0)
        line += QStringLiteral("，丢弃 %1 KiB（本机收得太慢，缓冲超过上限）")
                    .arg(droppedDelta / 1024);
    const QString error = audioPlayer_->lastError();
    if (!error.isEmpty())
        line += QStringLiteral("；上次错误：%1").arg(error);
    log(line);
}

// ---------------------------------------------------------------------------
// 占满 / 还原
//
// 「占满」= 该格跨满整个网格 + 其余格 `hide()`。三条要点：
// · **只 hide，不销毁** —— 隐藏的格子连接不断、帧照收，还原时立刻有画面
// （不会黑屏等第一帧）。这是 ViewGrid 的既有语义，这里只是驱动它。
// · 入口是**按格**的（点第 2 格的【占满】就占满第 2 格），与「当前选中」无关；
// Esc 是唯一「跟状态走」的入口，因为键盘事件本来就落在某一个具体控件上。
// · 占满 / 还原都**改变了格子尺寸** ⇒ 必须重算显示预算（见 scheduleDisplayBudget）。
// ---------------------------------------------------------------------------
void MainWindow::toggleMaximize(lanrdp::RemoteView *view)
{
    if (!view || !grid_)
        return;

    const int index = views_.indexOf(view);
    if (index < 0)
        return;

    // 一格时占满没有意义（格子本来就铺满）。按钮在 refreshViewUi 里已经置灰，
    // 这里是程序化路径（测试 / 将来可能的快捷键）的兜底。
    if (views_.size() <= 1)
        return;

    const bool wasMaximizedHere = grid_->isMaximized() && grid_->maximizedIndex() == index;
    grid_->setMaximizedIndex(wasMaximizedHere ? -1 : index);

    // 占满之后把键盘焦点交给画面 —— 用户这时最想按的是 Esc（退出占满）。
    // 点按钮进来时焦点在按钮上，而 Esc 的拦截只装在画面控件上 ⇒ 不交给它，
    // 按 Esc 会毫无反应，用户只能再去点【还原】。
    // ⚠️ 只在「进入占满」这一支做：还原时把焦点抢回画面没有道理。
    if (!wasMaximizedHere)
        view->renderer()->setFocus(Qt::OtherFocusReason);

    syncMaximizeState();
    updateSmoothDeadZone();   // 占满/还原会改变「有几个可见」⇒ 死区平滑开关跟着走
    scheduleDisplayBudget();
    log(wasMaximizedHere ? QStringLiteral("已还原多格布局")
                         : QStringLiteral("第 %1 格已占满").arg(index + 1));
}

void MainWindow::restoreMaximize(lanrdp::RemoteView *view)
{
    if (!grid_ || !view)
        return;
    if (!grid_->isMaximized() || grid_->maximizedIndex() != views_.indexOf(view))
        return;

    grid_->setMaximizedIndex(-1);
    syncMaximizeState();
    updateSmoothDeadZone();
    scheduleDisplayBudget();
    log(QStringLiteral("已还原多格布局（Esc）"));
}

// 权威状态在 `ViewGrid`（它决定谁铺满、谁 hide），`RemoteView` 只是镜像
//（按钮文案 + Esc 的判定）。凡可能改动格数或占满索引的地方都要调一次。
void MainWindow::syncMaximizeState()
{
    const int  maxIndex    = (grid_ && grid_->isMaximized()) ? grid_->maximizedIndex() : -1;
    const bool canMaximize = (views_.size() > 1);

    for (int i = 0; i < views_.size(); ++i) {
        lanrdp::RemoteView *v = views_.at(i);
        if (!v)
            continue;
        v->setMaximized(i == maxIndex);
        v->maximizeButton()->setEnabled(canMaximize);
    }
}

// ---------------------------------------------------------------------------
// 【被控】菜单 —— 被控端全局动作的家
//
// 被控不是「模式」，而是随时可以开、随时可以关的**全局功能**（控制格不受它影响）：
// · 开始监听：先弹【开始监听…】对话框确认端口与口令，确定后才开始；
// · 收回操作权：双态（收回 ⇄ 恢复），只作用于**本机被连入**这条路，
//   与控制格上「我这端有没有操作权」是两回事。
// ---------------------------------------------------------------------------

void MainWindow::buildHostMenu()
{
    QMenu *menu = menuBar()->addMenu(QStringLiteral("被控"));

    listenAction_ = menu->addAction(QStringLiteral("开始监听…"));
    listenAction_->setObjectName(QStringLiteral("actionListen"));
    listenAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
    listenAction_->setStatusTip(QStringLiteral("本机开始等待连接：先确认端口和口令，再开始监听"));
    connect(listenAction_, &QAction::triggered, this, [this] {
        if (isHosting())
            stopHost();
        else
            openHostConfigDialog();
    });

    takeInputAction_ = menu->addAction(QStringLiteral("收回操作权"));
    takeInputAction_->setObjectName(QStringLiteral("actionTakeInput"));
    takeInputAction_->setStatusTip(QStringLiteral(
        "立刻收回对方对本机的键鼠控制并保持锁定；再点一次恢复。锁定不影响文件传输"));
    // 收回操作权**不设**「监听中才可用」的门：未监听时也能预防性锁定
    // —— 先把键鼠锁住，之后谁连上来都操作不了（stopHost 会清锁）。
    connect(takeInputAction_, &QAction::triggered, this, &MainWindow::onTakeInputButtonClicked);

    refreshHostMenu();
}

// 把两个双态菜单项按当前状态刷新。由 updateConnectionUi 统一驱动
//（监听起停 / 输入锁定 / 解锁的每一条路径都会走到那里）。
void MainWindow::refreshHostMenu()
{
    if (!listenAction_ || !takeInputAction_)
        return;

    const bool hosting = isHosting();
    const bool locked  = hostState_.inputLocked;

    listenAction_->setText(hosting ? QStringLiteral("停止监听")
                                   : QStringLiteral("开始监听…"));
    listenAction_->setStatusTip(hosting
        ? QStringLiteral("停止监听：断开所有已连接的控制端，本机不再接受新连接")
        : QStringLiteral("本机开始等待连接：先确认端口和口令，再开始监听"));

    takeInputAction_->setText(locked ? QStringLiteral("恢复操作")
                                     : QStringLiteral("收回操作权"));
}

bool MainWindow::hostSessionActive() const
{
    return isHosting();
}

bool MainWindow::clientSessionActive() const
{
    // **任意一格**有会话在跑就算「控制会话活动中」。
    // `connected()` 的口径是「TCP 曾连上（含未握手）」—— `connectTo()` 里 Session 是
    // **同步**建好的，所以「点【连接】即算在跑」没有空档期。
    // 连不上退到重连待命也算「在跑」。
    // 两个用途：状态栏统计口径（被控端推送让位给控制端收帧）与剪贴板目标切换
    //（双角色同时在线时以控制端为准）。
    for (lanrdp::RemoteView *v : std::as_const(views_)) {
        auto *c = connectionOf(v);
        if (c && (c->connected() || c->reconnectPending()))
            return true;
    }
    return false;
}


// 把「各格自己的状态」刷到界面上。
//
// 由 updateConnectionUi() 末尾统一驱动 —— 那里是所有状态变化的收口点（连接建立、
// 断开、握手失败、重连排队、自动重连开关…），不必到处埋调用点。
void MainWindow::refreshViewUi()
{
    for (int i = 0; i < views_.size(); ++i) {
        lanrdp::RemoteView *v = views_.at(i);
        if (!v)
            continue;

        // 每格看**自己的连接**，三种状态：
        // · 空（没有连接对象）→ 空态层（「连接对方 / 让对方连进来」两个入口）；
        // · 断线（有记下的对端、但没连着）→ 只留出口（【重连】/【关闭】）。
        //   反例（真机上撞到的）：断线格上仍摆着【发送文件】【让出操作权】【断开】，
        //   一控多时条只有半屏宽，按钮把右端的出口挤出可见区域 ⇒ 那一格关不掉，
        //   用户看到的就是「界面还是两格」。
        // · 活着 → 完整的一套控制端控件。
        const bool stale = viewIsDisconnected(v);
        const bool live  = (connectionOf(v) != nullptr) && !stale;

        // 【连接另一台】= 只加一格空视窗，不弹对话框 ——「连出去还是让对方连进来」
        // 由用户新格的空态里再选。空格自己就是等连接的地方，不重复摆这个入口。
        v->connectButton()->setVisible(live);
        v->tokenStatusButton()->setVisible(live);
        v->releaseTokenButton()->setVisible(live);
        v->sendFileButton()->setVisible(live);
        // 【屏幕】= 被控端有多块屏时才出现。单屏被控端上它与没有这项功能时一模一样，
        // 窄格里也不会多挤掉一个按钮位置（一控多 2×2 时每格只有半个窗口宽）。
        {
            const auto *conn = connectionOf(v);
            const bool multi = conn && conn->hasScreenList()
                               && conn->screenList().screens.size() > 1;
            v->screenButton()->setVisible(live && multi);
            if (live && multi) {
                const lanrdp::ScreenList &list = conn->screenList();
                v->screenButton()->setToolTip(
                    QStringLiteral("选择要看的屏幕：共 %1 块，当前是第 %2 块")
                        .arg(list.screens.size())
                        .arg(int(list.current) + 1));
            }
        }
        v->disconnectButton()->setVisible(live);
        // 格数 < 2 时置灰（「能不能占满」由 syncMaximizeState 统一刷）。
        v->maximizeButton()->setVisible(live);

        // 条标题：写「这一格是谁」。控制格用对端地址（若干格之后一眼就知道哪台是哪台），
        // 还没连过就写「第 N 格」。
        const auto *c = connectionOf(v);
        QString     t;
        if (c && !c->host().isEmpty())
            t = QStringLiteral("%1:%2").arg(c->host()).arg(c->port());
        else if (views_.size() > 1)
            t = QStringLiteral("第 %1 格（未连接）").arg(i + 1);
        else
            t = QStringLiteral("未连接");

        // 断线格在标题上也要看得出来 —— 画面本身停了，只靠画面
        // 分不出「断了」与「远端没动」。退避与彻底断开措辞不同。
        if (stale)
            t += c->reconnectPending() ? QStringLiteral("（正在重连…）")
                                       : QStringLiteral("（已断开）");
        v->setTitle(t);

        // 空态层：只对「没有连接对象」的格出现 —— 那才是「接下来做什么」的问题所在。
        // 断线格在等重连 / 等用户处置，摆两个入口反而像"还没开始"。
        // 入口按钮随监听状态换双态（允许 ⇄ 禁止），与【被控】菜单同一份事实。
        v->emptyState()->setVisible(connectionOf(v) == nullptr);
        v->emptyState()->setHostListening(hostState_.listening);

        lanrdp::ScreenRenderer *r = v->renderer();
        // 「点击画面，即可操作对方电脑」那层提示只对**有连接**的格子有意义。
        // 空格点下去更糟：会把整窗键鼠交给一个没有连接的渲染器（本机点击消失）。
        r->setInputHintEnabled(live);
        if (connectionOf(v)) {
            r->setIdleText(QStringLiteral("等待远程画面…"));
        } else {
            // 空格的引导由空态层自己排版，画面控件这里不再画文字 ——
            // 两处都画会在按钮后面透出一行重复的提示。
            r->setIdleText(QString());
        }

        // 断线格的展示 —— 与条标题同一处判断（说的都是「这一格现在
        // 是什么状态」）。三条一起：
        // · 画面**保留最后一帧**并按「已过期」灰显（`setDisconnected` 内部转给渲染器）；
        // · 条上出现两个出口【重连】/【关闭】（其余按钮已在上面按 `stale` 收起）；
        // · 说明文字按「在等自动重连」还是「已经断了」分开措辞。
        // ⚠️ 这里是这两个按钮显隐的**唯一**写入点 —— 两个地方各写一半，迟早会有一处
        // 忘了同步（面板那轮就是被这种"两处各写一半"坑过的）。
        auto *vc = connectionOf(v);
        v->setDisconnected(
            stale,
            (vc && vc->reconnectPending())
                ? QStringLiteral("连接已断开，正在自动重连…\n（条上可【重连】立刻再试，或【关闭】这一格）")
                : QStringLiteral("已断开：画面停在最后一帧\n（条上可【重连】或【关闭】这一格）"));
        v->retryButton()->setVisible(stale);
        // 【关闭】的出口有两类：断线格（处置一条死连接）与空格（不想要这个窗口了）。
        // 唯一的空格不显示 —— 中央区总得留一格，关了它等于关掉软件的入口。
        const bool empty = (vc == nullptr);
        v->closeCellButton()->setVisible(stale || (empty && views_.size() > 1));

        // 条上的状态点。颜色只是辅助 —— 标题里那行「（已断开）」仍然保留，
        // 只靠颜色区分状态对色弱用户等于没有信息。
        lanrdp::ScreenOverlayBar::Status dot = lanrdp::ScreenOverlayBar::Status::Idle;
        if (vc) {
            if (stale) {
                dot = vc->reconnectPending()
                          ? lanrdp::ScreenOverlayBar::Status::Reconnecting
                          : lanrdp::ScreenOverlayBar::Status::Disconnected;
            } else if (vc->connected()) {
                dot = lanrdp::ScreenOverlayBar::Status::Connected;
            }
        }
        v->bar()->setStatus(dot);
    }

    // 「谁是占满格 / 能不能占满」和格子状态同一批刷 —— 触发条件有重叠
    //（格数一变，入口的可用性与占满索引的合法性都可能跟着变）。
    syncMaximizeState();
    updateSmoothDeadZone();
}

// 只有一格可见（本就一格 / 占满）时，允许渲染器在死区也开平滑。
// 死区关平滑是为了省多路的绘制开销；一格独享时那个 CPU 没人要，关掉反而让
// 2560→1100 这类常见缩小退化成最近邻、文字发虚。要跟「可见格数」走，所以单列。
void MainWindow::updateSmoothDeadZone()
{
    const bool solo = (views_.size() <= 1) || (grid_ && grid_->isMaximized());
    for (lanrdp::RemoteView *v : std::as_const(views_)) {
        if (v && v->renderer())
            v->renderer()->setSmoothInDeadZone(solo);
    }
}

// 「视图」菜单 —— 悬浮条的显隐入口。
//
// 条会遮住画面顶部约 30 逻辑像素（本机 4K@150% 下是 45 物理像素），远端窗口的
// 标题栏 / 最大化按钮可能因此点不到；给一个 Ctrl+L 的收起入口，比让用户去拖窗口强。
void MainWindow::buildViewMenu()
{
    QMenu *view = menuBar()->addMenu(QStringLiteral("视图"));

    QAction *toggle = view->addAction(QStringLiteral("悬浮操作条"));
    toggle->setObjectName(QStringLiteral("actionToggleOverlayBar"));
    toggle->setCheckable(true);
    toggle->setChecked(true);
    toggle->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    toggle->setStatusTip(QStringLiteral("显示 / 隐藏画面顶部的悬浮操作条（Ctrl+L）"));
    // 逐格隐藏 —— 一控多时「收起条」是全局偏好，不该只收起选中那一格。
    // 用 m_barsHidden 记一份是必须的：新加的格要继承这个偏好（见 addView -> refreshViewUi）。
    connect(toggle, &QAction::toggled, this, [this](bool on) {
        barsHidden_ = !on;
        for (lanrdp::RemoteView *v : std::as_const(views_)) {
            if (v && v->bar())
                v->bar()->setVisible(on);
        }
    });

    QMenu *settings = menuBar()->addMenu(QStringLiteral("设置"));
    QAction *open = settings->addAction(QStringLiteral("设置…"));
    open->setObjectName(QStringLiteral("actionOpenSettings"));
    open->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    open->setStatusTip(QStringLiteral("断线自动重连 / 远程键鼠总开关"));
    connect(open, &QAction::triggered, this, &MainWindow::openSettingsDialog);

    // 音频：**全局偏好**，所以入口只能在这里（与另两项同规矩，悬浮条上不挂）。
    // 默认开 —— 默认关会造成「明明连着却听不见」的困惑，而听不听是随时可关的。
    audioAction_ = settings->addAction(QStringLiteral("接收对方的声音"));
    audioAction_->setObjectName(QStringLiteral("actionReceiveAudio"));
    audioAction_->setCheckable(true);
    audioAction_->setChecked(audioEnabled_);
    audioAction_->setStatusTip(
        QStringLiteral("听到对方电脑的系统声音（一控多时只播当前选中那一格）"));
    connect(audioAction_, &QAction::toggled, this, [this](bool on) {
        audioEnabled_ = on;
        applyAudioRouting();
        setStatusText(on ? QStringLiteral("已开启对方的声音")
                         : QStringLiteral("已关闭对方的声音"));
    });
}

// 窗口尺寸变了 -> 重新下发显示预算（解码输出尺寸上限框）。
void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    pushDisplayBudget();
}

// 首次显示时中央区尺寸才落定，补一次显示预算下发（幂等）。
// （resizeEvent 也会触发，但顺序不保证。）
void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    pushDisplayBudget();
}

// 把「显示预算」下发给每一格连接的解码器。
//
// 口径变成**格子尺寸**（每格自己的 renderer 尺寸）：一控多时 4 路都按
// 窗口尺寸解码等于白烧 4 倍带宽 —— 这正是它的意义所在。1 格的场合两者相同
//（格子就是整个中央区），所以这条路对既有行为没有影响。
//
// ⚠️ 预算必须是**设备（物理）像素**：paintEvent 里 drawImage 的目标矩形走逻辑坐标，
// 而 QPainter 会按 devicePixelRatio 放大到设备像素绘制 —— 预算只给逻辑尺寸的话，
// 4K@150% 上等于把画面再放大 1.5 倍，肉眼可见变糊（比不缩还差）。
// 给物理像素 ⇒ 解码结果永远 ≥ 实际绘制尺寸，只缩不放，质量不退化。
//
// ⚠️⚠️ 还没定档的一件事（留到用 bench_decode_multi 实测后再定，别凭感觉改）：
// `nv12ToArgb32Scaled` 的收益守卫是**面积比 ≤ 1/9**，落在 (1/9, 1/4] 这一带
// 既省不下来、又拿不到「不缩放」的好处。4K 屏上做 2×2 时单格正好是源的 1/4，
// 恰好踩在死区里 —— 于是「按格子取」在这档会退化成 no-op（安全，但没收益），
// 而「按窗口取」会让每次切布局都要等一帧新尺寸的画面。两条路的取舍要拿实测说话。
void MainWindow::pushDisplayBudget()
{
    for (lanrdp::RemoteView *v : std::as_const(views_)) {
        auto *c = connectionOf(v);
        if (!v || !c)
            continue;

        const QSize logical = v->renderer()->size();
        // 布局未落定（还没 show）时控件尺寸是默认值，别把预算压成几十像素
        if (logical.width() < 64 || logical.height() < 64)
            continue;

        const qreal dpr = v->renderer()->devicePixelRatioF();
        c->setDisplayBudget(QSize(qRound(logical.width() * dpr),
                                  qRound(logical.height() * dpr)));
    }
}

// 格子尺寸变了之后重算显示预算。
//
// ⚠️ 格子尺寸不是「改了立刻生效」的：`QGridLayout` 被 invalidate 之后，真正的几何
// 要等下一轮事件循环的 LayoutRequest 才落定 ⇒ 当场读 `renderer->size()` 拿到的
// 是**旧值**。所以同步来一次（幂等、对尺寸没变的格子无害），再排队来一次拿新值。
// 这条路径不能靠 `MainWindow::resizeEvent` —— 窗口本身没有变尺寸，收不到它。
void MainWindow::scheduleDisplayBudget()
{
    pushDisplayBudget();
    QTimer::singleShot(0, this, [this] { pushDisplayBudget(); });
}

// 把**这一格**画面上的键鼠 / 拖拽接到这一格那条连接。
// 「跟随来源」而不是「跟随选中」：用户是在第 2 格的画面上按的键，那就该发给第 2 路
// —— 事件本身已经指明了是哪一路，用「当前选中格」反而会发错。
void MainWindow::wireRendererInput(lanrdp::RemoteView *view)
{
    if (!view)
        return;
    lanrdp::ScreenRenderer *renderer = view->renderer();

    connect(renderer, &lanrdp::ScreenRenderer::mouseMoved, this, [this, view](quint16 nx, quint16 ny) {
        auto *c = connectionOf(view);
        if (!c)
            return;
        lanrdp::InputEvent ev;
        ev.kind = lanrdp::InputKind::MouseMove;
        ev.nx   = nx;
        ev.ny   = ny;
        // 鼠标移动不是「操作」，只是「想知道能不能操作」的探针：没有输入权时
        // 一律不发（也不去申请令牌）。否则机主锁定输入后，控制端在画面上划一圈
        // 就会打出一片「令牌请求被拒」——那恰恰是控制端去踩机主的锁，观感极差。
        // 真正发起申请的是按键/点按（见 KeyPress / MouseButton 那两条）。
        if (c->hasToken())
            c->sendInput(ev);
    });

    connect(renderer, &lanrdp::ScreenRenderer::mouseButtonChanged, this, [this, view](int button, bool pressed, quint16 nx, quint16 ny) {
        auto *c = connectionOf(view);
        if (!c)
            return;
        lanrdp::InputEvent ev;
        ev.kind    = lanrdp::InputKind::MouseButton;
        ev.pressed = pressed;
        ev.nx      = nx;
        ev.ny      = ny;
        switch (button) {
        case Qt::LeftButton:   ev.button = lanrdp::MouseButtonId::Left;   break;
        case Qt::RightButton:  ev.button = lanrdp::MouseButtonId::Right;  break;
        case Qt::MiddleButton: ev.button = lanrdp::MouseButtonId::Middle; break;
        default:               return;  // 侧键等暂不支持
        }
        // 抬起（pressed == false）不可门控 —— 见 ClientConnection::sendInput。
        c->sendInput(ev);
    });

    connect(renderer, &lanrdp::ScreenRenderer::mouseWheel, this, [this, view](int delta, quint16 nx, quint16 ny) {
        auto *c = connectionOf(view);
        if (!c)
            return;
        lanrdp::InputEvent ev;
        ev.kind       = lanrdp::InputKind::MouseWheel;
        ev.wheelDelta = static_cast<std::int16_t>(delta);
        ev.nx         = nx;
        ev.ny         = ny;
        c->sendInput(ev);
    });

    connect(renderer, &lanrdp::ScreenRenderer::keyChanged, this, [this, view](int vk, bool pressed, bool extended) {
        auto *c = connectionOf(view);
        if (!c)
            return;
        lanrdp::InputEvent ev;
        ev.kind  = pressed ? lanrdp::InputKind::KeyDown : lanrdp::InputKind::KeyUp;
        ev.vk    = static_cast<std::uint16_t>(vk);
        ev.flags = extended ? lanrdp::InputFlagExtended : lanrdp::InputFlagNone;
        c->sendInput(ev);
    });

    // 未启用键鼠时画面中央那层提示被点了一下 —— 等价于去【设置…】勾上总开关。
    // 这是「输入被静默丢弃」那条可用性缺陷的出口：用户不必知道开关藏在哪。
    connect(renderer, &lanrdp::ScreenRenderer::inputEnableRequested, this, [this, view] {
        // 只认「这一格真的连着」。预览格 / 断线格没有下游，被点到时直接拒绝 ——
        // 否则总开关一开，整个窗口的键鼠都归一个没人接的渲染器（本机点击消失）。
        if (inputEnabled_ || !connectionOf(view))
            return;
        setInputEnabledForTest(true);
    });

    // 把文件拖到画面上即发送（与「发送文件…」按钮同一条发送路径）。
    // 路径按**这一格**走 —— 把文件拖到哪一格就发给那一台。
    connect(renderer, &lanrdp::ScreenRenderer::fileDropped, this, [this, view](const QString &path) {
        const QString name = QFileInfo(path).fileName();

        auto *c = connectionOf(view);
        if (!c || !c->ready()) {
            log(QStringLiteral("尚未连接到对端，无法发送拖入的文件：%1").arg(name));
            return;
        }

        log(QStringLiteral("拖入文件，准备发送：%1").arg(path));
        if (!c->sendFile(path))
            log(QStringLiteral("发送未启动（文件不可读，或连接已断开）"));
    });
}

// 把**这一格**那条连接对象的信号接到本窗口与该格。
// 分工没变：连接对象只管「网络与状态」，本窗口只管「画到哪一格 + 刷界面」。
// 参数化之后，N 格跑的是同一份代码（这正是把连接容器化的目的）。
void MainWindow::wireConnection(lanrdp::RemoteView *view)
{
    auto *c = connectionOf(view);
    if (!c)
        return;

    connect(c, &lanrdp::ClientConnection::logMessage,
            this, [this](const QString &line) { log(line); });
    connect(c, &lanrdp::ClientConnection::statusMessage,
            this, [this](const QString &text) { setStatusText(text); });
    connect(c, &lanrdp::ClientConnection::stateChanged,
            this, [this]() { updateConnectionUi(); });
    connect(c, &lanrdp::ClientConnection::connectionLost,
            this, [this, view]() { handleClientDisconnected(view); });

    // 令牌：继续转发成本窗口原有的 tokenStateChanged（对外信号），界面刷新则逐格走。
    connect(c, &lanrdp::ClientConnection::tokenChanged,
            this, [this, view](bool on) {
                emit tokenStateChanged(on);
                updateTokenStatus();     // 每格看自己那条连接的令牌状态
                updateClipboardTarget();

                // 令牌丢失（被收回 / 空闲让出）也是一次「中间态被打断」：
                // 此刻用户可能正按着键，必须补发抬起，否则那个键会卡在远端。
                // 只补发**这一格** —— 别的格没被收权。
                if (!on && view)
                    view->renderer()->releaseAllPressed();
            });

    connect(c, &lanrdp::ClientConnection::handshakeFinished,
            this, [this, view](bool ok, const QString &message) {
                if (ok) {
                    statsMode_ = QStringLiteral("client");
                    updateTokenStatus();     // 此时多半仍是“等待中”
                    if (inputEnabled_) {
                        log(QStringLiteral("远程键鼠控制已开启，点一下画面即可操作远端"));
                    } else {
                        log(QStringLiteral("提示：【设置…】里勾选「启用远程键鼠控制」后即可操作远端"));
                    }
                    setStatusText(QStringLiteral("已连接，等待画面…"));
                    updateConnectionUi();      // 连通后该格「断开」「发送文件」才可用
                    updateClipboardTarget();   // 控制端同步目标 = 这条会话
                    // 条标题要显示对端地址 —— 刷新交给 refreshViewUi()（它逐格读连接）。
                    if (auto *cc = connectionOf(view))
                        log(QStringLiteral("已连上 %1:%2").arg(cc->host()).arg(cc->port()));
                } else {
                    updateConnectionUi();
                }
                Q_UNUSED(view);
                emit handshakeResult(ok, message);
            });

    connect(c, &lanrdp::ClientConnection::frameReady,
            this, [this, view](const QImage &image, qint64 bytes, const QSize &sourceSize) {
                // 画面画到**来源那一格**（事件/帧自带来源，不看选中）。
                view->renderer()->setFrame(image);
                statsMode_ = QStringLiteral("client");
                // 分辨率字样由帧自身上报（不依赖渲染器）。
                // `image` 现在可能已被解码器等比缩小，**必须**用 sourceSize
                // （码流真实尺寸）——写成 image.size() 会让状态栏分辨率莫名变小。
                // 状态栏是全局的，于是只记「用户当下在看的那一格」的口径。
                if (view == selectedView())
                    lastFrameSize_ = sourceSize;
                emit frameReceived(sourceSize, bytes);
            });

    connect(c, &lanrdp::ClientConnection::clipboardReceived,
            this, [this](const lanrdp::Frame &frame) {
                if (clipboardSync_)
                    clipboardSync_->handleFrame(frame);
            });

    connect(c, &lanrdp::ClientConnection::inputSent,
            this, [this](int kind) { emit inputSent(kind); });

    // ---- 文件发送：进度条 / 状态栏文案（UI 部分留在这里）----
    // 进度条落在状态栏，全局只有一条 —— 一控多时「同时发两个文件」不在此范围，
    // 两个传输撞上会共用这一条（真实使用里也不会这么干）。
    connect(c, &lanrdp::ClientConnection::fileStarted,
            this, [this](const QString &name, quint64 total) {
                log(QStringLiteral("开始发送文件：%1（%2）").arg(name, humanBytes(total)));
                setStatusText(QStringLiteral("发送中：%1").arg(name));
                beginProgress(sendProgress_, QStringLiteral("发送中"), name, total);
            });

    auto lastTick = std::make_shared<QElapsedTimer>();
    connect(c, &lanrdp::ClientConnection::fileProgress,
            this, [this, lastTick](quint64 sent, quint64 total) {
                if (total == 0)
                    return;
                if (sent < total && lastTick->isValid() && lastTick->elapsed() < 200)
                    return;
                lastTick->restart();
                stepProgress(sendProgress_, sent, total);
                setStatusText(QStringLiteral("发送中 %1 / %2 (%3%)")
                                  .arg(humanBytes(sent))
                                  .arg(humanBytes(total))
                                  .arg(int(sent * 100 / total)));
            });

    connect(c, &lanrdp::ClientConnection::fileFinished,
            this, [this](bool ok, const QString &path, const QString &message) {
                endProgress(sendProgress_);
                if (ok) {
                    log(QStringLiteral("文件已送达：%1").arg(QFileInfo(path).fileName()));
                    setStatusText(QStringLiteral("文件发送完成"));
                } else {
                    log(QStringLiteral("文件发送失败：%1").arg(message));
                    setStatusText(QStringLiteral("文件发送失败"));
                }
                emit fileSent(ok, path, message);
            });

    // ---- 音频（对方的声音）：接上但**不按选中动态接线** ----
    // 每格都接着，真正「该不该出声」由 isAudioSource() 在收到时判。
    // 切格是高频操作，动态 connect/disconnect 一旦漏一次就是「这一格永远没声音」
    // 这种看不见的状态；而每块 20 ms 一次判等，代价可以忽略。
    connect(c, &lanrdp::ClientConnection::audioConfigReceived,
            this, [this, view](const lanrdp::AudioFormat &format) {
                if (isAudioSource(view))
                    audioPlayer_->begin(format);
            });
    connect(c, &lanrdp::ClientConnection::audioDataReceived,
            this, [this, view](qint64, const QByteArray &pcm) {
                if (isAudioSource(view))
                    audioPlayer_->push(pcm);
            });

    // ---- 多屏：屏列表变了就重刷这一格的条（显隐 + 悬停提示）----
    // 不做本地推测：显不显示、勾哪一项，全部以这一份列表为准。
    connect(c, &lanrdp::ClientConnection::screenListChanged,
            this, [this, view](const lanrdp::ScreenList &) {
                if (view)
                    refreshViewUi();
            });
}

void MainWindow::log(const QString &line)
{
    // 单点收口 —— 内存缓冲（供测试/内部读）+ 追加落盘 + 控制台。
    logPort_->append(line);
}

void MainWindow::setStatusText(const QString &text)
{
    if (statusLabel_)
        statusLabel_->setText(text);
    statusBar()->showMessage(text);
}

// ---------------------------------------------------------------------------
// 三个对话框的入口
// ---------------------------------------------------------------------------

void MainWindow::openHostConfigDialog()
{
    // 已在监听时不弹对话框：确定会先停掉上一轮再听（`HostController::start` 的
    // 幂等语义），会**踢掉所有已连入的控制端**。误触重启太危险，直接说明现状 ——
    // 真想改参数，先走【被控 → 停止监听】再开始。
    if (isHosting()) {
        log(QStringLiteral("本机已在监听，先停止监听才能修改端口或口令"));
        setStatusText(QStringLiteral("本机已在监听（先停止监听才能修改参数）"));
        return;
    }

    lanrdp::HostConfigDialog::Config cfg;
    cfg.port       = hostPort_;
    cfg.password   = hostPassword_;
    cfg.receiveDir = hostRecvDir_;
    cfg.useH264    = hostUseH264_;

    lanrdp::HostConfigDialog dlg(cfg, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const auto picked = dlg.config();
    hostPort_     = picked.port;
    hostPassword_ = picked.password;
    hostRecvDir_  = picked.receiveDir.trimmed().isEmpty()
                        ? lanrdp::defaultReceiveDir()
                        : picked.receiveDir.trimmed();
    setHostCodec(picked.useH264, hostBitrate_);

    // 记住这套参数，下次打开对话框直接预填（口令在其中，ini 里是 DPAPI 密文）。
    // 写不进去要**说出来**：静默失败会让用户以为"记住了"，下次开程序发现又空了却
    // 查不出线索 —— 而那正是他抱怨过的那件事。
    if (sessionStore_.isEnabled()) {
        lanrdp::HostSession saved;
        saved.port       = hostPort_;
        saved.password   = hostPassword_;
        saved.receiveDir = hostRecvDir_;
        saved.useH264    = hostUseH264_;
        if (!sessionStore_.saveHost(saved)) {
            log(QStringLiteral("⚠ 无法保存会话参数（%1），下次启动需要重新填写。")
                    .arg(sessionStore_.filePath()));
        }
    }

    // 监听 / 参数落定都由 startHost() 内部完成。用户在这里取消的话，
    // 什么都不会发生 —— 本函数在确定之前不碰任何状态。
    startHost(hostPort_, hostPassword_);
}

// 只加一格**空视窗**（内显空态两入口），不弹任何对话框。
// 「连接另一台」的新语义：先把格子摆出来，连出去 / 让对方连进来由用户新格的
// 空态里再选 —— 想连别人与想被连这两件事在同一处开始，不必预先猜。
void MainWindow::addEmptyView()
{
    if (views_.size() >= kMaxViews) {
        log(QStringLiteral("已达到 %1 个画面上限，请先关闭一个画面再连接下一台").arg(kMaxViews));
        return;
    }
    addView();
    updateConnectionUi();
}

void MainWindow::openConnectDialog()
{
    lanrdp::ConnectDialog::Config cfg;
    cfg.host     = peerHost_;
    cfg.port     = peerPort_;
    cfg.password = peerPassword_;

    lanrdp::ConnectDialog dlg(cfg, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const auto picked = dlg.config();
    peerHost_     = picked.host;
    peerPort_     = picked.port;
    peerPassword_ = picked.password;

    if (sessionStore_.isEnabled()) {
        lanrdp::PeerSession saved;
        saved.host     = peerHost_;
        saved.port     = peerPort_;
        saved.password = peerPassword_;
        if (!sessionStore_.savePeer(saved)) {
            log(QStringLiteral("⚠ 无法保存会话参数（%1），下次启动需要重新填写。")
                    .arg(sessionStore_.filePath()));
        }
    }

    startConnect(peerHost_, peerPort_, peerPassword_);
}

void MainWindow::openSettingsDialog()
{
    lanrdp::SettingsDialog::Config cfg;
    cfg.autoReconnect = autoReconnectEnabled();
    cfg.inputEnabled  = inputEnabled_;

    lanrdp::SettingsDialog dlg(cfg, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const auto picked = dlg.config();

    // 远程键鼠总开关：与原复选框 toggled 是同一条路径（全局状态 + 注入给每条连接）。
    if (picked.inputEnabled != inputEnabled_)
        setInputEnabledForTest(picked.inputEnabled);

    if (picked.autoReconnect != autoReconnectEnabled())
        onAutoReconnectToggled(picked.autoReconnect);
}

// 把「启用远程键鼠控制」总开关注入到**每一格**（连接 + 渲染器）。
//
// ⚠️ 必须**逐格**注入 —— 只改选中格的话，别的格会一直以为自己还开着 / 关着。
// ⚠️ 关掉时渲染器会**自己补发全部抬起的键**（见 ScreenRenderer::setInputEnabled），
//    否则用户正按住的 Ctrl 会永久卡在远端。
void MainWindow::setInputEnabledForTest(bool on)
{
    if (inputEnabled_ == on)
        return;
    inputEnabled_ = on;

    for (lanrdp::RemoteView *v : std::as_const(views_)) {
        if (!v)
            continue;
        if (auto *c = connectionOf(v))
            c->setInputEnabled(inputEnabled_);
        v->renderer()->setInputEnabled(inputEnabled_);
    }

    log(inputEnabled_ ? QStringLiteral("已启用远程键鼠控制：点一下画面即可操作远端")
                      : QStringLiteral("已停用远程键鼠控制"));
}

// 主动断开当前控制端连接（画面停更、令牌释放、界面复位）。
// 这也是「中止自动重连」的出口 —— 用户点了就得真的停，不能又连回来。
// 网络侧动作全部交给 ClientConnection；本窗口只负责日志与界面复位
// （复位由 connectionLost 信号驱动，见 wireClientConnection）。
// 无参版 = 断开**选中格**（保留是为了程序化入口 / 测试按名字调用）。
void MainWindow::onDisconnectButtonClicked()
{
    disconnectView(selectedView());
}

// 断开**指定格**的那条连接。条上的【断开】按格调用它（用户可能是在
// 第 2 格的条上点的，那当然该断第 2 路）。
void MainWindow::disconnectView(lanrdp::RemoteView *view)
{
    auto *c = connectionOf(view);
    // 「无活会话 且 无在途重连」= 没什么可断的，静默返回（与重构前的早退等价）。
    // 反过来说：「重连待命、但已经没有活着的会话」这一种必须走到下面 ——
    // 它只体现在停掉退避定时器上，界面若不主动复位，用户点完看不出任何反馈
    // （手工验证时捞出的：点完【断开】界面不复位）。
    if (!c || (!c->connected() && !c->reconnectPending()))
        return;

    log(QStringLiteral("已断开与被控端的连接"));
    c->disconnectNow();
}

// ---------------------------------------------------------------------------
// 断线格的两个出口
//
// 断线（对端关了 / 网断了 / 用户点了【断开】）之后，这一格**不清画面** —— 停在最后一帧
// 并灰显，同时给出两条路：
// · 【重连】= 用记下的参数立刻再试（不等退避计时）。语义与「再点一次【新连接…】」
// 相同，只是参数从连接自己那儿来，用户不必重填。
// · 【关闭】= 扔掉这一格。多格时格与连接一起消失；只剩一格时格不能删（中央区总得
// 有东西），退化为「扔掉连接 + 复位成空态」。
// ⚠️ 两条都必须**只动这一格** —— 一控多下「整体复位」的后果是：
// 断一台把别台的状态一起刷掉，用户会以为全断了。
// ---------------------------------------------------------------------------
void MainWindow::retryView(lanrdp::RemoteView *view)
{
    auto *c = connectionOf(view);
    if (!c || c->host().isEmpty())
        return;   // 没有记下的参数可重试

    // 重连前不画面 —— 保留最后一帧直到新画面到，用户至少知道断开前远端长什么样。
    c->reconnectNow();
    updateConnectionUi();
}

void MainWindow::closeView(lanrdp::RemoteView *view)
{
    if (!view || !grid_)
        return;

    const int index = views_.indexOf(view);
    if (index < 0)
        return;

    // 关格 = 一次「中间态被打断」：用户可能正按着键。先把抬起补发出去，
    // 再拆连接 —— 顺序反了就发不出去了（连接没了就没处发）。
    view->renderer()->releaseAllPressed();

    // 先拆连接（会 emit connectionLost -> handleClientDisconnected，同步跑完）。
    if (auto *c = view->connection())
        c->disconnectNow();

    if (views_.size() <= 1) {
        // 只剩一格：格不能删（`setCentralWidget()` 上的东西总得有）。
        // 退化为「扔掉这条连接、复位成空态」—— 与【断开】的区别就在这儿：
        // 【断开】留着连接对象与最后一帧（还能重连），【关闭】把整格清干净。
        view->takeConnection();
        view->renderer()->clear();
        view->setDisconnected(false);
        updateConnectionUi();
        log(QStringLiteral("已关闭唯一的视窗，复位为未连接状态"));
        return;
    }

    // 选中跟着「格」走，不跟着「下标」走：关掉的如果**不是**选中格，选中该留在原处
    //（`setViews` 已按视窗身份把选中迁到了它的新下标）。只有关掉的正是选中格时，
    // 才需要往旁边挪一格 —— 不然关掉末尾那格会让选中悄悄跳到另一台上去。
    const bool closedWasSelected = (grid_->selectedIndex() == index);

    views_.removeAt(index);
    view->takeConnection();   // 交还所有权 -> 随即销毁
    view->hide();
    view->deleteLater();

    QVector<QWidget *> ws;
    ws.reserve(views_.size());
    for (lanrdp::RemoteView *v : std::as_const(views_))
        ws.append(v);
    grid_->setViews(ws);
    if (closedWasSelected)
        grid_->setSelectedIndex(qBound(0, index - 1, views_.size() - 1));
    onGridSelectionChanged(grid_->selectedIndex());

    syncMaximizeState();
    scheduleDisplayBudget();   // 格数变了，剩下每格的尺寸也跟着变
    updateConnectionUi();
    log(QStringLiteral("已关闭一格视窗，剩余 %1 格").arg(views_.size()));
}

// 「断线格」= 有记下的对端、但此刻没连着。两个边界：
// · 刚 new 出来、还没 connectTo 过的连接对象（host 为空）⇒ 不是断线，是「未连接」空态；
// · 正在等自动重连的 ⇒ **算**（用户看到的就是画面停了，两个出口对它同样适用）。
bool MainWindow::viewIsDisconnected(const lanrdp::RemoteView *view) const
{
    if (!view)
        return false;
    const auto *c = view->connection();
    if (!c || c->host().isEmpty())
        return false;
    return !c->connected();
}

// 被控端的「已连 N 个」弹出菜单 —— 挂在悬浮条最左那颗按钮上
// （内容：踢出 / 持令牌者可收回输入权 / 锁定切换）。
void MainWindow::onClientListMenu(QToolButton *btn)
{
    if (!btn)
        return;

    const QVector<lanrdp::HostController::ClientInfo> clients =
        hostState_.clients;

    QMenu menu(this);
    if (clients.isEmpty()) {
        QAction *none = menu.addAction(QStringLiteral("暂无控制端连接"));
        none->setEnabled(false);
    } else {
        for (const lanrdp::HostController::ClientInfo &info : clients) {
            QMenu *sub = menu.addMenu(info.isHolder
                                          ? QStringLiteral("● %1（可操作本机）").arg(info.peer)
                                          : QStringLiteral("○ %1").arg(info.peer));
            QAction *kick = sub->addAction(QStringLiteral("踢出该控制端"));
            // 快照里的 peer 一并捕获 —— 菜单弹出期间那条连接可能已经断了，
            // 到那时再去问会话就会踩到悬空指针。
            const quint64 target = info.sessionId;
            const QString peer   = info.peer;
            connect(kick, &QAction::triggered, this, [this, target, peer] {
                if (host_)
                    // 只传**会话 id**：宿主在自己的线程里按 id 查池，那条连接早已
                    // 断开也只是 no-op（不像裸指针那样可能踢错人）。
                    postToHost([target](lanrdp::HostController *h) { h->dropClient(target); });
                // 只是**请求**：dropClient 交给宿主线程异步执行，对方可能早已
                // 自行断开（那时按 id 查池是 no-op）。日志不能说成已经踢掉了。
                log(QStringLiteral("已请求踢出控制端：%1").arg(peer));
            });
            if (info.isHolder) {
                QAction *take = sub->addAction(QStringLiteral("收回操作权"));
                connect(take, &QAction::triggered, this, &MainWindow::takeInput);
            }
        }
    }

    menu.addSeparator();
    const bool locked = hostState_.inputLocked;
    QAction *lock = menu.addAction(locked ? QStringLiteral("恢复操作")
                                          : QStringLiteral("收回操作权并锁定"));
    connect(lock, &QAction::triggered, this, &MainWindow::onTakeInputButtonClicked);

    menu.exec(btn->mapToGlobal(QPoint(0, btn->height())));
}

// 选屏菜单：**内容 = 对端最近一次广播的屏列表**，当前那块打勾。
//
// 抽成独立函数（而不是内联在点击回调里）有两个理由：
// 1) 「菜单项与列表一致」这件事能被单测直接验证，不必去捕一个正在模态弹出的菜单；
// 2) 菜单弹出的瞬间列表可能又变了（对端切屏广播），构造与触发都在同一份快照上。
QMenu *MainWindow::createScreenMenu(lanrdp::RemoteView *view)
{
    if (!view)
        return nullptr;
    const auto *conn = connectionOf(view);
    if (!conn || !conn->hasScreenList())
        return nullptr;

    const lanrdp::ScreenList list = conn->screenList();
    if (list.screens.size() < 2)
        return nullptr;

    auto *menu = new QMenu(view->screenButton());
    for (int i = 0; i < list.screens.size(); ++i) {
        const lanrdp::ScreenInfo &screen = list.screens.at(i);
        QString text = QStringLiteral("屏 %1 · %2×%3")
                           .arg(i + 1)
                           .arg(screen.width)
                           .arg(screen.height);
        if (screen.primary)
            text += QStringLiteral(" · 主屏");
        QAction *action = menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(i == int(list.current));
        // 只发**请求**：真正生效与否看对端回来的新列表（本端不做乐观更新，
        // 否则切屏失败时界面会一直停在「我以为切过去了」的状态）。
        // 触发时才去问连接 —— 菜单弹出期间这一格可能被关掉 / 换了连接，
        // 捕获裸指针会在那时变成悬空。
        connect(action, &QAction::triggered, this, [this, view, i] {
            auto *conn = connectionOf(view);
            if (!conn || !conn->hasScreenList())
                return;
            if (i >= conn->screenList().screens.size())
                return;   // 列表在弹出期间变短了（对端拔了一块屏）
            conn->selectScreen(i);
        });
    }
    return menu;
}

// 清空日志。只清**内存缓冲**，绝不动落盘文件。
// 文件是排查素材，不能被一个入口清掉 —— 它只受尺寸滚动控制。
void MainWindow::clearLog()
{
    logPort_->clearBuffer();
}

// 输入路径整体转交 ClientConnection（含令牌门控与「用操作本身申请令牌」）。
// 全局键鼠开关（inputEnabled_）是本窗口的状态，通过 setInputEnabled 注入给连接对象。
void MainWindow::sendInputEvent(const lanrdp::InputEvent &ev, bool force)
{
    if (auto *c = activeConnection())
        c->sendInput(ev, force);
}

void MainWindow::releaseToken()
{
    if (auto *c = activeConnection())
        c->releaseToken();
}

// ---------------------------------------------------------------------------
// 与宿主线程的交互
//
// 宿主跑在**自己的工作线程**上（`host_->moveToThread(hostThread_)`），所以本窗口只有两种合法姿势：
// · 投递一份工作过去（fire-and-forget），或
// · 读它推过来的**快照**（`hostState_`）。
// 直接读 / 写宿主的成员都是跨线程，一律不允许。
//
// 「谁是令牌持有者」这个问题本窗口也不再回答 —— 宿主手里的 `Session *` 不能搬到
// GUI 侧来用（socket 也有线程归属）。剪贴板因此改成「把帧投递回去，由宿主决定发给谁」。
// ---------------------------------------------------------------------------

void MainWindow::postToHost(std::function<void(lanrdp::HostController *)> work)
{
    if (!host_ || !work)
        return;
    lanrdp::HostController *h = host_;
    // 用 lambda 形式的 invokeMethod：编译期检查，且不必为每个动作声明一个槽。
    QMetaObject::invokeMethod(h, [h, work = std::move(work)] { work(h); }, Qt::QueuedConnection);
}

bool MainWindow::hostStartSync(quint16 port, const QString &password)
{
    // ⚠️ 阻塞投递的前提是宿主线程**正在跑事件循环**；线程停了还投递会永久死锁。
    if (!host_ || !hostThread_ || !hostThread_->isRunning())
        return false;

    // 启动参数与 start() 一起在工作线程里落定 —— 分两次投递会让「参数生效」与
    // 「开始监听」之间出现一个可被观察到的中间态。
    const QString recvDir = receiveDir();
    const bool    useH264 = hostUseH264_;
    const int     bitrate = hostBitrate_;

    bool                    ok = false;
    lanrdp::HostController *h  = host_;
    // `&ok` 是**本函数栈上**的变量、被另一个线程写 —— 这里安全：BlockingQueued
    // 保证 invokeMethod 返回时那段 lambda 已经执行完毕。
    QMetaObject::invokeMethod(
        h,
        [h, port, password, recvDir, useH264, bitrate, &ok] {
            h->setReceiveDir(recvDir);
            h->setCodec(useH264, bitrate);
            ok = h->start(port, password);
        },
        Qt::BlockingQueuedConnection);
    return ok;
}

void MainWindow::hostStopSync()
{
    if (!host_ || !hostThread_ || !hostThread_->isRunning())
        return;
    lanrdp::HostController *h = host_;
    QMetaObject::invokeMethod(h, [h] { h->stop(); }, Qt::BlockingQueuedConnection);
}

void MainWindow::refreshClientList()
{
    updateHostTokenUi();       // 与列表内容无关，未监听时也要能正确复位
    updateClipboardTarget();   // 令牌易主 / 释放 / 断开都收口在这里

    // 统计口径跟着「有没有控制端在收」走：连上 -> 推送中，最后一个走 -> 清空。
    // 本函数是这两件事的必经之路（宿主任何列表变化都会推一帧 `uiStateChanged` 快照）。
    if (hostState_.clientCount > 0 && !clientSessionActive())
        statsMode_ = QStringLiteral("host");
    else if (statsMode_ == QLatin1String("host"))
        statsMode_.clear();

    // 「已连 N 个」逐格刷到悬浮条最左（未监听时全部隐藏）。
    // 按钮上只放数字（人形图标在左边），整句话留给悬停提示与无障碍名字。
    const QVector<lanrdp::HostController::ClientInfo> clients =
        hostState_.clients;
    const int count = clients.size();

    QString tip;
    if (count == 0) {
        tip = QStringLiteral("暂无控制端连接");
    } else {
        // 明细走提示（悬停即可看），点开菜单才是操作 —— 避免为了看一眼谁在线就弹菜单。
        QStringList lines;
        for (const lanrdp::HostController::ClientInfo &info : clients) {
            lines << (info.isHolder
                          ? QStringLiteral("● %1（当前可操作本机）").arg(info.peer)
                          : QStringLiteral("○ %1").arg(info.peer));
        }
        if (hostState_.inputLocked)
            lines << QStringLiteral("（操作权已锁定：任何控制端都操作不了本机）");
        tip = QStringLiteral("%1\n点击查看可用操作：断开指定控制端 / 收回操作权")
                  .arg(lines.join(QStringLiteral("\n")));
    }

    for (lanrdp::RemoteView *v : std::as_const(views_)) {
        if (!v)
            continue;
        QToolButton *btn = v->clientListButton();
        if (!btn)
            continue;
        btn->setVisible(hostState_.listening);
        btn->setText(QString::number(count));
        btn->setAccessibleName(QStringLiteral("已连 %1 个控制端").arg(count));
        btn->setEnabled(count > 0);
        btn->setToolTip(tip);
    }
}

// 把「键鼠是否已锁定」同步到【被控】菜单的双态项上
// （否则机主自己也会困惑为什么控制端点不动）。
void MainWindow::updateHostTokenUi()
{
    refreshHostMenu();
}

// 收回远程输入权。
// 三段式顺序不能反：① 先上锁 ② 再摘掉持有者 ③ 最后通知对端。
// 反过来（先释放）会留一个「刚释放 -> 对方 request -> 又拿到」的竞态窗口，
// 功能会时灵时不灵 —— 这正是「只 release 不 lock」不能用的原因。
// 收回远程输入权。
// 三段式（lock -> release -> 通知对端）的顺序理由、列表刷新、对端答复
// 都在 `HostController::takeInput()` 里 —— 本窗口只转发，文案由宿主的
// `logLine` / `statusText` 信号送回来。
void MainWindow::takeInput()
{
    postToHost([](lanrdp::HostController *h) { h->takeInput(); });
}

// 解锁 —— 恢复「无人持有即授予」。
void MainWindow::unlockInput()
{
    postToHost([](lanrdp::HostController *h) { h->unlockInput(); });
}

void MainWindow::onTakeInputButtonClicked()
{
    if (hostState_.inputLocked)
        unlockInput();
    else
        takeInput();
}

// 操作权状态图标与【让出操作权】逐格刷 —— 每格看**自己那条连接**的令牌。
// 一控多时各连各的令牌（远端被控端那边是「谁先要谁得」），
// 拿全局一份去显示必然显示错。
void MainWindow::updateTokenStatus(bool hasToken)
{
    Q_UNUSED(hasToken);   // 只为兼容 tokenStateChanged(bool) 的连接，实际逐格读

    for (lanrdp::RemoteView *v : std::as_const(views_)) {
        if (!v)
            continue;

        auto *c            = connectionOf(v);
        const bool holds   = c && c->hasToken();
        const bool ready   = c && c->ready();

        v->setTokenState(holds   ? lanrdp::RemoteView::TokenState::Held
                          : ready ? lanrdp::RemoteView::TokenState::Waiting
                                  : lanrdp::RemoteView::TokenState::NotConnected);
        v->releaseTokenButton()->setEnabled(holds);
    }
}

void MainWindow::sendMouseMove(quint16 nx, quint16 ny)
{
    lanrdp::InputEvent ev;
    ev.kind = lanrdp::InputKind::MouseMove;
    ev.nx   = nx;
    ev.ny   = ny;
    sendInputEvent(ev, /*force=*/true);
}

void MainWindow::sendMouseButton(int button, bool pressed)
{
    lanrdp::InputEvent ev;
    ev.kind    = lanrdp::InputKind::MouseButton;
    ev.pressed = pressed;
    // 归一化坐标随便给个中心值：这条路径要的是「有按键动作」，位置无关紧要。
    ev.nx = 32768;
    ev.ny = 32768;
    switch (button) {
    case 0:  ev.button = lanrdp::MouseButtonId::Left;   break;
    case 1:  ev.button = lanrdp::MouseButtonId::Right;  break;
    case 2:  ev.button = lanrdp::MouseButtonId::Middle; break;
    default: return;
    }
    sendInputEvent(ev, /*force=*/true);
}

void MainWindow::setHostCodec(bool useH264, int bitrateBps)
{
    // 本窗口留一份，是为了【开始监听…】对话框回填；真正生效的那份在宿主里。
    hostUseH264_ = useH264;
    hostBitrate_ = bitrateBps;
    postToHost(
        [useH264, bitrateBps](lanrdp::HostController *h) { h->setCodec(useH264, bitrateBps); });
}

// ---------------------------------------------------------------------------
// 文件传输
// ---------------------------------------------------------------------------

QString MainWindow::receiveDir() const
{
    const QString text = hostRecvDir_.trimmed();
    if (!text.isEmpty())
        return text;
    return lanrdp::defaultReceiveDir();
}

void MainWindow::setReceiveDir(const QString &dir)
{
    if (dir.isEmpty())
        return;
    // 同上：本窗口留一份供对话框回填，实际落盘目录由宿主持有。
    hostRecvDir_ = dir;
    postToHost([dir](lanrdp::HostController *h) { h->setReceiveDir(dir); });
}

// 接收文件的落盘与接线已整体搬进 `HostController`（`ensureFileReceiver()` 在那里）；
// 本窗口只保留上面那三段「事件 -> 文案 / 进度条」的转译。

bool MainWindow::sendFile(const QString &path)
{
    // FileSender 由连接对象持有并封装；进度 / 结果通过
    // fileStarted / fileProgress / fileFinished 信号回到本窗口（见 wireConnection）。
    // 公开入口走**选中格**（与其它程序化入口一致）。
    auto *c = activeConnection();
    return c && c->sendFile(path);
}

void MainWindow::halfCloseClientWhenFlushed()
{
    auto *c = activeConnection();
    QTcpSocket *sock = (c && c->session()) ? c->session()->socket() : nullptr;
    if (!sock || sock->state() != QAbstractSocket::ConnectedState)
        return;

    connect(sock, &QTcpSocket::disconnected, this, [this]() { emit clientClosed(); });

    auto halfClose = [sock]() {
        if (sock->state() != QAbstractSocket::ConnectedState)
            return;
        // 只关写方向：FIN 排在已排队的数据之后，且不像 closesocket() 那样
        // 在读缓冲非空时回 RST（RST 会把还没被确认的发送队列一起丢掉）。
        ::shutdown(static_cast<SOCKET>(sock->socketDescriptor()), SD_SEND);
    };

    if (sock->bytesToWrite() == 0) {
        halfClose();
        return;
    }
    // 还有没交给内核的就等它排空 —— FIN 必须排在数据后面。
    // 重复进来只是再调一次 shutdown()，第二次是空操作。
    connect(sock, &QTcpSocket::bytesWritten, this, [sock, halfClose](qint64) {
        if (sock->bytesToWrite() == 0)
            halfClose();
    });
}

void MainWindow::onSendFileClicked()
{
    sendFileFrom(selectedView());
}

// 从**指定格**发文件 —— 条上的【发送文件…】按格调用它。
// 把文件发给哪一台，由「你在哪一格的条上点的」决定，不是「哪一格被选中」。
void MainWindow::sendFileFrom(lanrdp::RemoteView *view)
{
    auto *c = connectionOf(view);
    if (!c || !c->ready()) {
        log(QStringLiteral("尚未连接到对端，先点「新连接…」再发文件"));
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要发送的文件"));
    if (path.isEmpty())
        return;

    log(QStringLiteral("准备发送：%1").arg(path));
    if (!c->sendFile(path))
        log(QStringLiteral("发送未启动（文件不可读，或连接已断开）"));
}

void MainWindow::startHost(quint16 port, const QString &password)
{
    // 记住参数，供下次打开【开始监听…】对话框时回填。
    if (port != 0)
        hostPort_ = port;
    if (!password.isNull())
        hostPassword_ = password;

    // 口令必须合规才允许开始监听。这是**兜底**：对话框点确定时已经校验过一次，
    // 但命令行（--host）与程序化调用都直接走到这里。
    if (!lanrdp::isValidPassword(password)) {
        log(QStringLiteral("口令不合规（至少 6 位，且只能用英文字母、数字和符号），未开始监听"));
        setStatusText(QStringLiteral("口令不合规，未开始监听"));
        return;
    }

    // 启动参数（接收目录 / 编码方式）在对话框里可能刚被改过。它们与 start()
    // **一起在工作线程里落定**（见 hostStartSync）—— 分两次投递会让「参数生效」
    // 与「开始监听」之间出现一个可被观察到的中间态。

    // 监听 / 会话池 / 抓屏编码 / 注入 / 文件落盘 / 令牌全部由宿主持有；
    // 起不来的原因（端口被占等）由 listenFailed 那一路记进日志。
    if (!hostStartSync(port, password)) {
        hostState_.listening = false;   // 同步落定：阻塞投递保证了「返回即已生效」
        setStatusText(QStringLiteral("监听失败"));
        updateConnectionUi();
        return;
    }
    hostState_.listening = true;

    updateConnectionUi();
}

// 停止监听 —— 断开全部控制端并保留窗口，可再次开始监听。
// 摘 target / 解锁 / 断会话 / 清半截文件 / 销毁监听器全在 `HostController::stop()`，
// 这里只剩界面复位。
void MainWindow::stopHost()
{
    // 早退判据也读快照 —— 宿主在工作线程上，问不得。
    if (!hostState_.listening)
        return;

    hostStopSync();
    hostState_.listening = false;

    statsMode_ = clientReady() ? QStringLiteral("client") : QString();
    refreshClientList();
    updateConnectionUi();
}

bool MainWindow::isHosting() const
{
    // 读**本地快照**：宿主是否在监听，只有快照说了算（跨线程读成员是不安全的）。
    return hostState_.listening;
}

// 对端断开 / 被踢 / 主动断开：复位界面。
// **只做 UI** —— 拆会话 / 停心跳 / 释放令牌等内核清理已归
// ClientConnection（它拆完会发 connectionLost，本函数由那条信号驱动，见 wireClientConnection）。
// 只复位**断开的那一格** —— 一控多时断了一台不该把别人的画面一起清掉。
void MainWindow::handleClientDisconnected(lanrdp::RemoteView *view)
{
    // **刻意不清画面**。断线后停在最后一帧并灰显（由 updateConnectionUi
    // 逐格判定后调 setDisconnected），用户还能看清远端断线前是什么样子。
    // 只有「换一台重连」时才清 —— 那是另一台机器，留着上一台的画面会误导
    // （见 startConnect 里的 clear()）。
    Q_UNUSED(view);

    // 状态栏统计口径：还有别格连着就继续按控制端算，否则退回被控端 / 清空。
    if (!clientReady()) {
        bool anyClient = false;
        for (lanrdp::RemoteView *v : std::as_const(views_)) {
            auto *c = connectionOf(v);
            if (c && c->connected()) {
                anyClient = true;
                break;
            }
        }
        if (!anyClient)
            statsMode_ = isHosting() ? QStringLiteral("host") : QString();
    }

    updateConnectionUi();
    updateClipboardTarget();  // 会话没了，同步目标退回被控端的令牌持有者
}

// 按当前状态刷新条上按钮的文案与可用性 —— **逐格**刷。
void MainWindow::updateConnectionUi()
{
    const bool canAddView = (views_.size() < kMaxViews);

    for (lanrdp::RemoteView *v : std::as_const(views_)) {
        if (!v)
            continue;
        auto *c = connectionOf(v);

        // 【连接另一台】= 再开一格空视窗的入口，只在格数到上限时失效。
        v->connectButton()->setEnabled(canAddView);

        // 重连待命时也要能点【断开】—— 那是用户中止重连的唯一入口；
        // 另外「TCP 连上了但还在等握手」这段时间 ready() 仍是 false，
        // 不把「已有会话」算进来就会出现「点不动断开」的死角。
        v->disconnectButton()->setEnabled(c && (c->ready()
                                                || c->reconnectPending()
                                                || c->connected()));
        v->sendFileButton()->setEnabled(c && c->ready());
    }

    // 逐格显隐（refreshViewUi）与【被控】菜单的双态项同一批刷新 ——
    // 触发条件有重叠（有没有会话在跑 / 监听起停 / 锁定切换），
    // 分两处写迟早会漏一处。
    refreshViewUi();
    refreshHostMenu();
}

// ---------------------------------------------------------------------------
// 剪贴板同步（仅文本）
// ---------------------------------------------------------------------------

void MainWindow::setupClipboard()
{
    clipboardSync_ = new lanrdp::ClipboardSync(this);

    // 拿不到 GUI 应用（纯 QCoreApplication 环境）时不装端口，同步器只发不收。
    if (auto *guiApp = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        clipboardSync_->setClipboard(guiApp->clipboard());

    connect(clipboardSync_, &lanrdp::ClipboardSync::sent, this, [this](const QString &text) {
        log(QStringLiteral("已发送剪贴板文本（%1 字符）").arg(text.size()));
    });
    connect(clipboardSync_, &lanrdp::ClipboardSync::received, this, [this](const QString &text) {
        log(QStringLiteral("已写入远端剪贴板文本（%1 字符）").arg(text.size()));
    });
    connect(clipboardSync_, &lanrdp::ClipboardSync::rejected, this, [this](const QString &reason) {
        log(QStringLiteral("剪贴板未同步：%1").arg(reason));
    });
}

// 同步目标的选择规则：
// ① 控制端已连上 -> 同步到被控端（此刻用户在操作远端）；
// ② 否则若本端正在监听且有人持令牌 -> 与被控端视角下的「令牌持有者」同步；
// ③ 都没有 -> nullptr，同步暂停（不缓存、不事后回放）。
// 单窗口双角色同时在线时以控制端为准：一帧只发给一个对面，避免两端互相覆盖。
void MainWindow::updateClipboardTarget()
{
    if (!clipboardSync_)
        return;

    // 控制端这一路用**选中格**的会话（用户当下在看哪一台，剪贴板就往哪一台同步）。
    lanrdp::Session *target = nullptr;
    if (auto *c = activeConnection(); clientReady() && c && c->session())
        target = c->session();
    clipboardSync_->setSession(target);

    // 被控端这一路**不能**把持有者的 `Session *` 交给同步器 —— 那条 socket 归工作线程，
    // 在这里调 `sendFrame` 就是跨线程碰 socket；而且持有者随时可能换人。
    // 改走「把帧投递回去，由宿主在自己的线程里决定发给谁」（无人持令牌时自然丢弃）。
    if (!target && host_) {
        lanrdp::HostController *h = host_;
        clipboardSync_->setFrameSink([h](const lanrdp::Frame &frame) {
            QMetaObject::invokeMethod(h, [h, frame] { h->sendClipboardFrame(frame); },
                                      Qt::QueuedConnection);
        });
    }
}

void MainWindow::startConnect(const QString &host, quint16 port, const QString &password)
{
    // 记住参数，供下次打开【新连接…】对话框时回填。
    if (!host.isEmpty())
        peerHost_ = host;
    if (port != 0)
        peerPort_ = port;
    if (!password.isNull())
        peerPassword_ = password;

    // ---- 挑一格来用（这就是「一控多」的入口）----
    // · 选中格空着（没连过 / 那条连接已死透）→ 复用它，等价于「换一台重连」；
    // · 否则再开一格（上限 4 格）；
    // · 到上限了 → 退回选中格，等于「把这一台换成那一台」（并写日志说明）。
    lanrdp::RemoteView *view = selectedView();
    if (!viewIsReusable(view)) {
        if (views_.size() < kMaxViews) {
            view = addView();
        } else {
            log(QStringLiteral("已达 %1 格上限，本次连接会替换当前选中格").arg(kMaxViews));
        }
    }
    if (!view)
        return;

    // 这一格第一次用：给它建连接对象，并把「不是连接属性」的全局偏好注入进去。
    // ⚠️ 注入这一步必须在 connectTo() 之前 —— 顺序反了，第一条心跳之前就建好的
    // 那些东西会拿不到开关（重连策略就是其中最明显的一个）。
    if (!view->connection()) {
        view->setConnection(std::make_unique<lanrdp::ClientConnection>());
        wireConnection(view);      // 该格连接 -> 本窗口与该格
        wireRendererInput(view);   // 该格画面的键鼠 / 拖拽 -> 该格连接
        view->connection()->setAutoReconnectEnabled(autoReconnectOn_);
        view->connection()->setInputEnabled(inputEnabled_);
        view->renderer()->setInputEnabled(inputEnabled_);
    }

    // 整条连接生命周期（建会话 / 握手 / 收帧 / 令牌 / 心跳 / 退避重连）都在
    // lanrdp::ClientConnection 里；本窗口只负责「清画面 + 转交参数」。
    view->renderer()->clear();   // 别把上一台的最后一帧留给这一台
    view->connection()->connectTo(host, port, password);

    // 格数可能刚变（新开一格），每格的尺寸也跟着变 —— 排队重算一次显示预算。
    scheduleDisplayBudget();
    updateConnectionUi();
}

void MainWindow::updateStats()
{
    if (statsMode_.isEmpty())
        return;

    const bool    hosting = (statsMode_ == QLatin1String("host"));
    // 控制端的收帧计数与帧尺寸已随连接搬进 ClientConnection。
    // 控制端这一路读**选中格**的连接 —— 状态栏讲的是「你正在看的那一路」。
    auto         *cc      = activeConnection();
    const quint64 frames  = hosting ? hostState_.framesSent
                                    : (cc ? cc->framesReceived() : 0);
    const quint64 bytes   = hosting ? hostState_.bytesSent
                                    : (cc ? cc->bytesReceived() : 0);

    // 差分基线必须**绑定统计来源**。
    // 换选中格 / 换角色时来源变了，而新来源的累计计数可能比上一来源**小**
    //（例：从「已收 5 帧」的那格切到「一帧没收到」的那格）——
    // 拿旧基线去减是无符号运算，会回绕成 1.8e19 fps 这种天文数字。
    QObject *source = hosting ? static_cast<QObject *>(host_)
                              : static_cast<QObject *>(cc);
    if (source != lastStatsSource_) {
        lastStatsSource_ = source;
        lastFrames_      = frames;
        lastBytes_       = bytes;
        lastFrameSize_   = QSize();   // 上一格的分辨率对新格没有意义
        return;                       // 这一拍只重建基线，不出数
    }

    const quint64 deltaFrames = frames - lastFrames_;
    const quint64 deltaBytes  = bytes - lastBytes_;
    const bool    changed     = (deltaFrames != 0 || deltaBytes != 0);

    lastFrames_ = frames;
    lastBytes_  = bytes;

    if (!changed)
        return;

    const double mbps = static_cast<double>(deltaBytes) * 8.0 / 1e6;

    // 分辨率字样改由**帧自身上报**，不再取 renderer_->frameSize()。
    // 被控端的本机预览默认关闭，渲染器里没有帧，继续用老办法会让状态栏
    // 永久缺掉这一栏（看起来像坏了）。帧尺寸由收到帧时统一记录。
    const QSize size = lastFrameSize_;

    QString text = QStringLiteral("%1 | %2x%3 | %4 fps | %5 Mbps | 累计 %6 帧")
                       .arg(hosting ? QStringLiteral("推送中") : QStringLiteral("接收中"))
                       .arg(size.width())
                       .arg(size.height())
                       .arg(deltaFrames)
                       .arg(mbps, 0, 'f', 1)
                       .arg(frames);

    const quint64 injected = hostState_.injectedCount;
    const quint64 failed   = hostState_.injectFailedCount;
    if (hosting && (injected > 0 || failed > 0)) {
        text += QStringLiteral(" | 注入 %1 次").arg(injected);
        if (failed > 0)
            text += QStringLiteral("（失败 %1）").arg(failed);
    }

    setStatusText(text);
}

// ---------------------------------------------------------------------------
// 心跳 + 断线自动重连（控制端侧）
// 实现已整体移入 lanrdp::ClientConnection —— 心跳发送与判死、退避重连、
// 连接参数记忆都在那里（它是「每条连接一份」的，一控多时天然可 N 份）。
// 本窗口只剩查询转发与「开关变化」这一个入口（由【设置…】驱动）。
// 开关升格为**全局偏好**（`autoReconnectOn_`，定义在头文件里内联），
// 新开的格从这里继承 —— 见 startConnect。
// ---------------------------------------------------------------------------

// 按**选中格**回答 ——「在等重连吗」这个问题只对某一条连接成立。
bool MainWindow::reconnectPending() const
{
    auto *c = activeConnection();
    return c && c->reconnectPending();
}

// 程序化入口（UI 上唯一编辑处是【设置…】对话框）。
// 直接复用开关槽，两条路径走的是同一份「关掉就停退避定时器」的逻辑。
void MainWindow::setAutoReconnectEnabled(bool on)
{
    onAutoReconnectToggled(on);
}

void MainWindow::onAutoReconnectToggled(bool on)
{
    // 这是**全局偏好** —— 记一份在本窗口（以后新开的格自动继承），
    // 并逐格注入（只改选中格的话，别的格会继续按老策略重连）。
    autoReconnectOn_ = on;

    for (lanrdp::RemoteView *v : std::as_const(views_)) {
        auto *c = connectionOf(v);
        if (!c)
            continue;
        c->setAutoReconnectEnabled(on);
        if (!on)
            c->cancelReconnect();   // 停退避定时器 + 清零计数（否则用户关不掉）
    }

    log(on ? QStringLiteral("已开启断线自动重连")
           : QStringLiteral("已关闭断线自动重连"));
    // 两个方向都要刷：updateConnectionUi()（经 refreshViewUi）是断线格文案与
    // 【重连】/【关闭】可见性的唯一写入者 —— 开启时同样要让 UI 与状态一致
    // （此前只有关闭分支刷，开启后这些控件会停在旧态）。
    updateConnectionUi();
}
