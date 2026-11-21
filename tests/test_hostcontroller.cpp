#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "host/HostController.h"
#include "host/ScreenCapturer.h"
#include "net/Auth.h"
#include "net/FileReceiver.h"
#include "net/Session.h"
#include "protocol/Frame.h"
#include "protocol/InputEvent.h"
#include "protocol/ScreenControl.h"

// 被控端宿主边界（host/HostController）的契约：监听 / 会话池快照 /
// 输入令牌 / 统计查询。全部走**真实 loopback**，不打桩。
//
// 这一层是 MainWindow 被控端逻辑下沉后的落点 —— 从前它内联在 MainWindow 里，
// 只能靠 UI 用例间接覆盖；现在可以直接断言。

namespace {

// 「响应时长」用例的前提是**采集本身很快**：采集与编码跑在宿主线程上，
// 往返时长的下界就是单帧成本。桌面越大，单帧成本越高；超过这个面积后，
// 几百毫秒是分辨率带来的，不是退化。约 2560×1560。
constexpr qint64 kMaxMeasuredPixels = 4000000;

// 桌面物理尺寸。测试进程通常是 DPI-unaware 的，而 `GetDeviceCaps` 会按
// 缩放后的坐标回值 —— 临时把当前线程切成 per-monitor aware 才量得到真实像素。
QSize desktopSize()
{
#ifdef Q_OS_WIN
    DPI_AWARENESS_CONTEXT prev =
        ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HDC dc = ::GetDC(nullptr);
    const int w = dc ? ::GetDeviceCaps(dc, DESKTOPHORZRES) : 0;
    const int h = dc ? ::GetDeviceCaps(dc, DESKTOPVERTRES) : 0;
    if (dc)
        ::ReleaseDC(nullptr, dc);

    if (prev)
        ::SetThreadDpiAwarenessContext(prev);
    return QSize(w, h);
#else
    return {};
#endif
}

// 当前会话能不能真的采到画面。
//
// 要判两件事，缺一都会假红：
//   ① 锁屏 / 切到别的用户时输入桌面不是 "Default"，此时采到的是登录画面；
//   ② 桌面**是**输入桌面也不等于采得到帧 —— 显示器休眠时 DXGI 的 DuplicateOutput
//      直接 E_ACCESSDENIED，而 ① 完全看不出来。只判 ① 的后果是：机器闲置几分钟后
//      依赖帧的用例开始假红（2026-11-17 20:06 实测，空闲 6 分钟即复现）。
bool screenCaptureIsPossible()
{
#ifdef Q_OS_WIN
    HDESK desk = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desk)
        return false;

    wchar_t name[128] = {};
    DWORD needed = 0;
    const BOOL ok =
        ::GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), &needed);
    ::CloseDesktop(desk);
    if (!ok || ::lstrcmpiW(name, L"Default") != 0)
        return false;
#endif

    // 真开一次抓屏：与 test_capture 用的是同一个判据，失败原因也就一致。
    lanrdp::ScreenCapturer capturer;
    QString                err;
    return capturer.init(&err);
}

// 把 logLine 的 QSignalSpy 摊平成文本行，便于断言与打印。
//
// ⚠️ 不要用「lambda 捕获局部的 QStringList」代替 QSignalSpy：HostController
// **析构时还会 emit 日志**，而捕获的容器声明在它之后 ⇒ 容器先被销毁，
// 那条日志就追加进了已析构的对象。症状是随机位置的堆损坏、崩在下一个用例，
// 排查极费劲（2026-11-18 用 PageHeap 抓到：~HostController → stop() → logLine）。
QStringList logLines(const QSignalSpy &spy)
{
    QStringList out;
    out.reserve(spy.size());
    for (const QList<QVariant> &args : spy)
        out.append(args.at(0).toString());
    return out;
}

} // namespace

class TestHostController : public QObject
{
    Q_OBJECT

private slots:
    void startsAndStopsListening();
    void reportsListenFailureOnBusyPort();
    void stopIsIdempotent();

    void authenticatedClientAppearsInClients();
    // 鉴权日志要带会话号与握手耗时
    void authenticatedLogCarriesSessionId();
    void clientDisconnectRemovesItFromList();

    void tokenRequestIsGrantedToFirstClient();
    void takeInputLocksAndDropsHolder();
    void unlockInputAllowsRequestsAgain();
    void stopClearsClientsAndLock();

    // 断开 / 收权时「补发抬起」必须也覆盖键盘 —— 卡住的修饰键会让被控端
    // 后续所有输入变形，而键盘没有任何自动恢复的途径。
    void holderDisconnectReleasesStuckKeys();

    void receiveDirRoundTrips();
    void codecChoiceIsRemembered();
    void statsStartAtZero();

    // 反向传文件（被控端 → 控制端）：必须有人持操作权才发，且要真落到对端
    // 反向发文件（被控端 → 控制端）
    void clipboardFilesGoToTheTokenHolder();
    void pickedFilesGoToTheTokenHolder();

    // 隐私遮挡：停流 + 收权；锁只在「这次是我加的」时才解开
    void privacyCurtainLocksInputAndSurvivesRestart();
    void privacyCurtainKeepsOwnersOwnLock();
    void privacyCurtainAlsoSilencesAudio();
    void privacyCurtainKeepsNewClientFromStartingAudio();

    // 线程亲和性的「单一决定点」：内部定时器只能在 start() 里建。
    void timersExistOnlyAfterStart();

    // ---- 线程契约（宿主被搬到工作线程之后）----
    void movedHostBuildsTimersOnWorkerThread();
    void previewIsGatedOnHostSide();
    void startStopRepeatedlyDoesNotCrash();
    void hostStaysResponsiveWhileStreaming();

    // 音频采集线程的生命周期：没人听不开、stop() 必须收干净（退出程序不挂）
    void audioCaptureFollowsClientsAndStopsWithHost();

    // ---- 多屏：状态由被控端广播，选屏只是请求 ----
    void screenListIsSentAfterAuth();
    void screenSelectAlwaysAcknowledgedWithNewList();
    void invalidScreenSelectDoesNotChangeOutput();
};

void TestHostController::startsAndStopsListening()
{
    lanrdp::HostController hc;
    QVERIFY(!hc.isListening());
    QCOMPARE(hc.port(), quint16(0));

    QSignalSpy listening(&hc, &lanrdp::HostController::listening);
    QVERIFY(hc.start(0, QStringLiteral("pw")));
    QVERIFY(hc.isListening());
    QVERIFY(hc.port() != 0);
    QCOMPARE(listening.count(), 1);

    hc.stop();
    QVERIFY(!hc.isListening());
    QCOMPARE(hc.clientCount(), 0);
}

void TestHostController::reportsListenFailureOnBusyPort()
{
    // 先用一个裸 QTcpServer 占住端口，再让 HostController 去监听同一个口。
    QTcpServer squatter;
    QVERIFY(squatter.listen(QHostAddress::Any, 0));
    const quint16 busy = squatter.serverPort();
    QVERIFY(busy != 0);

    lanrdp::HostController hc;
    QSignalSpy failed(&hc, &lanrdp::HostController::listenFailed);
    QVERIFY(!hc.start(busy, QStringLiteral("pw")));
    QCOMPARE(failed.count(), 1);
    QVERIFY(!hc.isListening());
}

void TestHostController::stopIsIdempotent()
{
    lanrdp::HostController hc;
    hc.stop();   // 没监听时 stop 应当安全
    QVERIFY(!hc.isListening());

    QVERIFY(hc.start(0, QStringLiteral("pw")));
    hc.stop();
    hc.stop();
    QVERIFY(!hc.isListening());
}

void TestHostController::authenticatedClientAppearsInClients()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));
    const quint16 port = hc.port();

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();

    QTRY_COMPARE(hc.clientCount(), 1);

    const auto list = hc.clients();
    QCOMPARE(list.size(), 1);
    QVERIFY2(!list.first().peer.isEmpty(), "对端标识（ip:port）必须可读");
    QVERIFY2(!list.first().isHolder, "刚连上还没请求令牌，不该是持有者");
}

// 鉴权日志要带会话号与握手耗时：排障时最常被问的就是「到底连上没有、等了多久」，
// 而这两条以前只能从时间戳去猜。
//
// ⚠️ 用 QSignalSpy 而不是「lambda 捕获局部的 QStringList」：HostController 析构时
// 还会 emit 日志，而局部容器的销毁顺序在它之前 —— 那样写会在用例**结束后**踩到
// 已析构的对象，症状是堆损坏、崩在下一个用例（排查极费劲）。
void TestHostController::authenticatedLogCarriesSessionId()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    QSignalSpy spy(&hc, &lanrdp::HostController::logLine);

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    QString authed;
    for (const QList<QVariant> &args : spy) {
        const QString line = args.at(0).toString();
        if (line.contains(QStringLiteral("鉴权")))
            authed = line;
    }
    QVERIFY2(!authed.isEmpty(), "应当有一条鉴权通过的日志");
    QVERIFY2(authed.contains(QStringLiteral("会话 #")), qPrintable(authed));
    QVERIFY2(authed.contains(QStringLiteral("握手")), qPrintable(authed));
    QVERIFY2(authed.contains(QStringLiteral("ms")), qPrintable(authed));
}

void TestHostController::clientDisconnectRemovesItFromList()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    auto *client = new lanrdp::Session(socket);
    lanrdp::AuthClient auth(client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    client->socket()->abort();
    delete client;

    QTRY_COMPARE(hc.clientCount(), 0);
    QVERIFY(hc.clients().isEmpty());
}

void TestHostController::tokenRequestIsGrantedToFirstClient()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));

    int granted = 0;
    QObject::connect(&client, &lanrdp::Session::frameReceived,
                     [&granted](const lanrdp::Frame &f) {
                         if (f.type == static_cast<std::uint8_t>(lanrdp::FrameType::TokenGrant))
                             ++granted;
                     });
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    lanrdp::Frame req;
    req.type = static_cast<std::uint8_t>(lanrdp::FrameType::TokenReq);
    client.sendFrame(req);

    QTRY_COMPARE(granted, 1);
    QTRY_VERIFY(hc.holderSession() != nullptr);
    QVERIFY(!hc.inputLocked());
    QVERIFY(hc.clients().first().isHolder);
}

// 控制端按住一个键之后连接断掉 —— 那一针「抬起」再也送不到，被控端必须自己补。
// 不补的症状：那个键永久停在按下态（Shift 卡住则后续全大写，Ctrl 卡住则点什么
// 都是组合键），而键盘没有任何「等一下自己就恢复」的可能。
//
// ⚠️ 本条走**真实注入**：注入不进去（锁屏 / 前台窗口权限更高）就 skip ——
// 那说明测的不是本类的逻辑。
void TestHostController::holderDisconnectReleasesStuckKeys()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    // 没令牌的输入会被门控丢掉，连注入都不会发生
    lanrdp::Frame req;
    req.type = static_cast<std::uint8_t>(lanrdp::FrameType::TokenReq);
    client.sendFrame(req);
    QTRY_VERIFY(hc.holderSession() != nullptr);

    // F24：任何程序都不绑定它，注入不干扰本机桌面。
    lanrdp::InputEvent down;
    down.kind = lanrdp::InputKind::KeyDown;
    down.vk   = 0x87;   // VK_F24

    lanrdp::Frame frame;
    frame.type    = static_cast<std::uint8_t>(lanrdp::FrameType::InputEvent);
    frame.payload = lanrdp::encodeInputEvent(down);
    client.sendFrame(frame);

    QTest::qWait(300);   // loopback 往返 + 注入
    if (hc.injectedCount() == 0)
        QSKIP("这一针没注入进去（锁屏 / 前台窗口权限更高），测不到本条逻辑");

    QSignalSpy spy(&hc, &lanrdp::HostController::logLine);

    // 抬起永远送不到：连接在这里断掉
    client.socket()->abort();
    QTRY_COMPARE(hc.clientCount(), 0);

    QString release;
    for (const QList<QVariant> &args : spy) {
        const QString line = args.at(0).toString();
        if (line.contains(QStringLiteral("补发")))
            release = line;
    }
    QVERIFY2(!release.isEmpty(), "断开时应当补发一条抬起日志");
    QVERIFY2(release.contains(QStringLiteral("按键")), qPrintable(release));
}

void TestHostController::takeInputLocksAndDropsHolder()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    lanrdp::Frame req;
    req.type = static_cast<std::uint8_t>(lanrdp::FrameType::TokenReq);
    client.sendFrame(req);
    QTRY_VERIFY(hc.holderSession() != nullptr);

    hc.takeInput();

    QVERIFY(hc.inputLocked());
    QVERIFY2(hc.holderSession() == nullptr, "收回后持有者必须为空");
    QVERIFY(!hc.clients().first().isHolder);
}

void TestHostController::unlockInputAllowsRequestsAgain()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    hc.takeInput();
    QVERIFY(hc.inputLocked());

    hc.unlockInput();
    QVERIFY(!hc.inputLocked());

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    int granted = 0;
    QObject::connect(&client, &lanrdp::Session::frameReceived,
                     [&granted](const lanrdp::Frame &f) {
                         if (f.type == static_cast<std::uint8_t>(lanrdp::FrameType::TokenGrant))
                             ++granted;
                     });
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    lanrdp::Frame req;
    req.type = static_cast<std::uint8_t>(lanrdp::FrameType::TokenReq);
    client.sendFrame(req);

    QTRY_COMPARE(granted, 1);
}

void TestHostController::stopClearsClientsAndLock()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    hc.takeInput();
    QVERIFY(hc.inputLocked());

    hc.stop();

    QVERIFY(!hc.isListening());
    QCOMPARE(hc.clientCount(), 0);
    QVERIFY2(!hc.inputLocked(), "停止监听必须一并解锁（否则留下谁也连不上的死锁态）");
    QVERIFY(hc.holderSession() == nullptr);
}

void TestHostController::receiveDirRoundTrips()
{
    lanrdp::HostController hc;
    hc.setReceiveDir(QStringLiteral("C:/temp/hc-recv"));
    QCOMPARE(hc.receiveDir(), QStringLiteral("C:/temp/hc-recv"));

    hc.setReceiveDir(QString());   // 空串被忽略（沿用上一次的值）
    QCOMPARE(hc.receiveDir(), QStringLiteral("C:/temp/hc-recv"));
}

void TestHostController::codecChoiceIsRemembered()
{
    lanrdp::HostController hc;
    QVERIFY(hc.usesH264());   // 默认开 H.264

    hc.setCodec(false, 4 * 1000 * 1000);
    QVERIFY(!hc.usesH264());
    QCOMPARE(hc.bitrate(), 4 * 1000 * 1000);

    // 还没监听时也要能记住（与从前 setHostCodec 的语义一致）。
    QVERIFY(hc.start(0, QStringLiteral("pw")));
    QVERIFY(!hc.usesH264());
    QCOMPARE(hc.bitrate(), 4 * 1000 * 1000);
}

void TestHostController::statsStartAtZero()
{
    lanrdp::HostController hc;
    QCOMPARE(hc.framesSent(), quint64(0));
    QCOMPARE(hc.bytesSent(), quint64(0));
    QCOMPARE(hc.injectedCount(), quint64(0));
    QCOMPARE(hc.injectFailedCount(), quint64(0));
    QVERIFY(hc.sourceInfo().isEmpty());
    QVERIFY(hc.codecInfo().isEmpty());
}

// 内部定时器（空闲判定 / 看门狗打点）必须**在 start() 里**创建，不能在构造函数里。
//
// 理由不是「构造里建就不工作」（`moveToThread` 会连带搬子对象并重启活动定时器），
// 而是把「线程亲和性」收成**一个决定点** + 让它可断言 —— 而且对象一旦有 parent
// 就完全不能 `moveToThread`。
void TestHostController::timersExistOnlyAfterStart()
{
    lanrdp::HostController hc;

    QVERIFY2(hc.findChildren<QTimer *>().isEmpty(),
             "构造函数里不该建 QTimer —— 亲和性要留到 start() 在工作线程内决定");

    QVERIFY(hc.start(0, QStringLiteral("pw")));
    // start() 之后至少两个：空闲判定 + 看门狗打点（HostServer 的心跳另算）。
    QVERIFY2(hc.findChildren<QTimer *>().size() >= 2,
             "start() 之后应当能看到内部定时器");

    hc.stop();
}

// ---------------------------------------------------------------------------
// 线程契约（宿主被搬到工作线程之后）
// ---------------------------------------------------------------------------

// `moveToThread` 之后，`start()` 建的定时器必须落在**工作线程**上。
// 这是「亲和性只有一个决定点」那条不变量的实际兑现 —— 靠的就是把定时器
// 从构造函数挪进了 start()。
void TestHostController::movedHostBuildsTimersOnWorkerThread()
{
    QThread worker;
    auto   *hc = new lanrdp::HostController();
    hc->moveToThread(&worker);
    connect(&worker, &QThread::finished, hc, &QObject::deleteLater);
    worker.start();

    bool ok = false;
    QMetaObject::invokeMethod(hc, [hc, &ok] { ok = hc->start(0, QStringLiteral("pw")); },
                              Qt::BlockingQueuedConnection);
    QVERIFY(ok);

    const auto timers = hc->findChildren<QTimer *>();
    QVERIFY2(!timers.isEmpty(), "start() 之后应当有内部定时器");
    for (QTimer *t : timers)
        QVERIFY2(t->thread() == &worker, "定时器必须属于工作线程，而不是创建 HostController 的那条线程");

    QMetaObject::invokeMethod(hc, [hc] { hc->stop(); }, Qt::BlockingQueuedConnection);
    worker.quit();
    QVERIFY(worker.wait(5000));
}

// 预览关着时**宿主侧就不发** `previewFrame`，但抓屏链路照旧跑（帧尺寸仍上报）。
// 这条守的是跨线程投递的积压隐患：4K 每帧独立缓冲 ≈33 MB，「发了让 GUI 丢掉」
// 在 GUI 被占住时会堆到 GB 级。
void TestHostController::previewIsGatedOnHostSide()
{
    // 锁屏 / 切到别的用户时采不到用户画面，`sizes` 永远等不到 —— 这条用例的正向
    // 前提（「抓屏确实在跑」）根本不成立。跳过而不是判红：它与
    // `hostStaysResponsiveWhileStreaming` 是同一个环境约束。
    if (!screenCaptureIsPossible())
        QSKIP("当前会话采不到画面（锁屏 / 切换用户 / 显示器休眠），跟抓屏有关的判据不成立");

    lanrdp::HostController hc;
    hc.setPreviewEnabled(false);
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    int previews = 0;
    int sizes    = 0;
    QObject::connect(&hc, &lanrdp::HostController::previewFrame,
                     [&previews](const QImage &) { ++previews; });
    QObject::connect(&hc, &lanrdp::HostController::frameSizeChanged,
                     [&sizes](const QSize &) { ++sizes; });

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));
    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    // 正向前提：抓屏确实在跑（否则「一帧没发」是空过的）。
    QTRY_VERIFY2(sizes > 0, "抓屏链路应当在跑 —— 帧尺寸应当有上报");
    QCOMPARE(previews, 0);

    hc.setPreviewEnabled(true);
    QTRY_VERIFY2(previews > 0, "打开预览之后应当开始送帧");
}

// 反复起停不崩、不漏（每次 start 都会重建监听器与定时器）。
void TestHostController::startStopRepeatedlyDoesNotCrash()
{
    lanrdp::HostController hc;
    for (int i = 0; i < 20; ++i) {
        QVERIFY(hc.start(0, QStringLiteral("pw")));
        QVERIFY(hc.isListening());
        hc.stop();
        QVERIFY(!hc.isListening());
    }
    QCOMPARE(hc.clientCount(), 0);
    QVERIFY(!hc.inputLocked());
}

// 被控端在推流时对输入类帧的响应有多慢 —— 量的是 `TokenReq -> TokenGrant` 的往返。
//
// 这条往返**必须穿过宿主线程**，所以它的上界就是「宿主线程被单次抓屏阻塞的时长」：
// `AcquireNextFrame` 在静止桌面会**等满超时**，而超时 = `qMin(intervalMs_, N)`。
// 因此本用例是那次「超时 100 ms → 15 ms」改动的**量化判据**（改前应当看到
// 明显更大的 max）。
//
// 但它的判据是**绝对毫秒**，只在「单帧成本远小于阈值」的桌面上成立：
// 采集与编码都在宿主线程，往返下界就是单帧成本，所以 4K 桌面下几百毫秒是
// 分辨率带来的常态，不是退化。环境不满足时跳过，但仍打印实测值。
void TestHostController::hostStaysResponsiveWhileStreaming()
{
    if (!screenCaptureIsPossible())
        QSKIP("当前会话采不到画面（锁屏 / 切换用户 / 显示器休眠），跟抓屏有关的判据不成立");

    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));
    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));

    int grants = 0;
    QObject::connect(&client, &lanrdp::Session::frameReceived, [&grants](const lanrdp::Frame &f) {
        if (f.type == static_cast<std::uint8_t>(lanrdp::FrameType::TokenGrant))
            ++grants;
    });
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    lanrdp::Frame req;
    req.type = static_cast<std::uint8_t>(lanrdp::FrameType::TokenReq);

    // 先要一次令牌 —— 完整客户端会自动做这件事，裸 Session 不会。
    //（拿到持有者身份后，下面的循环靠「同一持有者重复请求幂等」反复打同一条往返。）
    client.sendFrame(req);
    QTRY_VERIFY2(hc.holderSession() != nullptr, "应当先拿到一次令牌（其后抓屏才会真的跑起来）");

    QVector<qint64> rtts;
    for (int i = 0; i < 25; ++i) {
        const int before = grants;
        QElapsedTimer t;
        t.start();
        client.sendFrame(req);   // 同一持有者重复请求是**幂等**的 ⇒ 每次都会回 TokenGrant
        while (grants == before && t.elapsed() < 2000)
            QTest::qWait(1);
        if (grants == before)
            break;   // 超时说明链路坏了，交给下面的数量断言报错
        rtts.append(t.elapsed());
    }

    // 一次都没跑通 = 链路坏了，跟桌面大小无关，这条永远有效。
    QVERIFY2(!rtts.isEmpty(), "一次 TokenReq→TokenGrant 往返都没回来");

    std::sort(rtts.begin(), rtts.end());
    const qint64 med = rtts.at(rtts.size() / 2);
    const qint64 max = rtts.last();
    qInfo("host responsiveness while streaming: n=%d med=%lldms max=%lldms",
          static_cast<int>(rtts.size()), static_cast<long long>(med), static_cast<long long>(max));

    // 桌面面积超出判据适用范围时跳过：这里的数字反映分辨率（以及「exe 刚链接、
    // 缓存还冷」这类环境噪声），不反映退化。
    // ⚠️ 这个判断**必须排在样本数断言之前**：4K 桌面上每拍采集 + 编码本身就慢，
    // 根本凑不满 25 个样本，先断言样本数会把「本来就要跳过」的用例变成假红
    //（2026-11-17 实测：同一台机器上 HEAD 原码重编也只剩 2–3 个样本）。
    const QSize desk = desktopSize();
    if (qint64(desk.width()) * desk.height() > kMaxMeasuredPixels) {
        QSKIP(qPrintable(QStringLiteral(
            "桌面 %1×%2 上单帧采集+编码成本本身就有几百毫秒，绝对毫秒判据不适用"
            "（实测 n=%3 med=%4ms max=%5ms，仅供观察）")
            .arg(desk.width()).arg(desk.height()).arg(rtts.size()).arg(med).arg(max)));
    }

    QVERIFY2(rtts.size() >= 20, qPrintable(QStringLiteral("样本不足（%1/25）").arg(rtts.size())));
    QVERIFY2(max < 500, qPrintable(QStringLiteral("推流期间被控端响应退化到 %1 ms").arg(max)));
}

// 音频采集线程的生命周期。
//
// 两条都要守：① 没人听的时候**不开采集**（loopback 是一条常跑的设备流，
// 白占设备也白耗电）；② `stop()` 必须把线程收干净 —— 音频是「最容易把退出
// 拖死」的那类新增线程（设备调用可能长阻塞）。
void TestHostController::audioCaptureFollowsClientsAndStopsWithHost()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    QVERIFY2(!hc.audioCaptureRunning(), "没有控制端时不该开音频采集");

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    // 第一个控制端接入才起采集。没有音频设备的机器上它必然起不来 —— 那是环境事实。
    QElapsedTimer up;
    up.start();
    while (!hc.audioCaptureRunning() && up.elapsed() < 2000)
        QTest::qWait(50);
    if (!hc.audioCaptureRunning())
        QSKIP("本机没有可用的音频输出设备（采集线程起不来，只验停机路径）");

    QElapsedTimer timer;
    timer.start();
    hc.stop();
    const qint64 elapsed = timer.elapsed();

    QVERIFY2(elapsed < 3000,
             qPrintable(QStringLiteral("stop() 被音频线程拖住了 %1 ms").arg(elapsed)));
    QVERIFY2(!hc.audioCaptureRunning(), "stop() 之后采集线程必须已经退出");
    QVERIFY(!hc.isListening());
}

// ---- 多屏：状态由被控端广播，选屏只是请求 ----

// 鉴权通过后必须**先**拿到一份权威状态，否则控制端连「有几块屏」都不知道，
// 条上的【屏幕】入口无从显示。
void TestHostController::screenListIsSentAfterAuth()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    QVector<lanrdp::ScreenList> lists;
    QObject::connect(&client, &lanrdp::Session::frameReceived, &client,
                     [&lists](const lanrdp::Frame &frame) {
                         if (frame.type != static_cast<quint8>(lanrdp::FrameType::ScreenList))
                             return;
                         if (auto decoded = lanrdp::decodeScreenList(frame.payload))
                             lists.append(*decoded);
                     });

    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!lists.isEmpty(), 5000);

    const QVector<lanrdp::MonitorInfo> monitors = lanrdp::ScreenCapturer::enumerateOutputs();
    if (monitors.isEmpty())
        QSKIP("枚举不到任何已附着的显示输出（无桌面会话？），只验了「有帧」这一半");

    // 列表必须与真实枚举**逐项一致**：屏的数量、几何、主屏标记。
    QCOMPARE(lists.first().screens.size(), monitors.size());
    int primaries = 0;
    for (int i = 0; i < monitors.size(); ++i) {
        QCOMPARE(lists.first().screens.at(i).width, quint32(monitors.at(i).rect.width()));
        QCOMPARE(lists.first().screens.at(i).height, quint32(monitors.at(i).rect.height()));
        QCOMPARE(lists.first().screens.at(i).x, qint32(monitors.at(i).rect.x()));
        QCOMPARE(lists.first().screens.at(i).y, qint32(monitors.at(i).rect.y()));
        QCOMPARE(lists.first().screens.at(i).primary, monitors.at(i).isPrimary);
        primaries += monitors.at(i).isPrimary ? 1 : 0;
    }
    QCOMPARE(primaries, 1);
    // 首次接入默认看主屏
    QVERIFY(lists.first().currentScreen() != nullptr);
    QVERIFY(lists.first().currentScreen()->primary);
}

// 选屏是**请求**，答复永远是一份新状态 —— 即便「选了跟现在一样的那块」也要回。
// 没有这条，控制端就分不清「在途」还是「被忽略」，界面会一直显示旧状态。
void TestHostController::screenSelectAlwaysAcknowledgedWithNewList()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    QVector<lanrdp::ScreenList> lists;
    QObject::connect(&client, &lanrdp::Session::frameReceived, &client,
                     [&lists](const lanrdp::Frame &frame) {
                         if (frame.type != static_cast<quint8>(lanrdp::FrameType::ScreenList))
                             return;
                         if (auto decoded = lanrdp::decodeScreenList(frame.payload))
                             lists.append(*decoded);
                     });

    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!lists.isEmpty(), 5000);

    const int before = lists.size();
    client.sendFrame(static_cast<quint8>(lanrdp::FrameType::ScreenSelect),
                     lanrdp::encodeScreenSelect(0));
    QTRY_VERIFY_WITH_TIMEOUT(lists.size() > before, 5000);
    QCOMPARE(lists.last().current, quint8(0));
}

// 越界的 index（那块屏被拔掉了 / 控制端记的是旧列表）不许改状态，
// 但**必须**回一份新列表把控制端拉回真实状态 —— 不要静默丢弃。
void TestHostController::invalidScreenSelectDoesNotChangeOutput()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    QVector<lanrdp::ScreenList> lists;
    QObject::connect(&client, &lanrdp::Session::frameReceived, &client,
                     [&lists](const lanrdp::Frame &frame) {
                         if (frame.type != static_cast<quint8>(lanrdp::FrameType::ScreenList))
                             return;
                         if (auto decoded = lanrdp::decodeScreenList(frame.payload))
                             lists.append(*decoded);
                     });

    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!lists.isEmpty(), 5000);

    const quint8 beforeCurrent = lists.first().current;
    const int    beforeIndex   = hc.screenOutputIndex();
    const int    beforeCount   = lists.size();

    // 本机多半只有一块屏；用一个必定越界的序号（15），保证走的是「无效」分支。
    client.sendFrame(static_cast<quint8>(lanrdp::FrameType::ScreenSelect),
                     lanrdp::encodeScreenSelect(15));
    QTRY_VERIFY_WITH_TIMEOUT(lists.size() > beforeCount, 5000);

    QCOMPARE(lists.last().current, beforeCurrent);
    QCOMPARE(hc.screenOutputIndex(), beforeIndex);
}

// 反向传文件（被控端 → 控制端）：机主复制的那批只发给**正在操作本机**的人。
//
// 两个判据：
// ① 没人持操作权 ⇒ 一帧都不发。把机主的剪贴板推给一个只看着屏幕的人，
//    等于绕过「谁在操作」这条线 —— 这是本功能唯一的安全口径。
// ② 有人持操作权 ⇒ 文件真的落到对端，内容逐字节一致（sha 校验在 FileReceiver 里做）。
void TestHostController::clipboardFilesGoToTheTokenHolder()
{
    QTemporaryDir recvDir;
    QVERIFY(recvDir.isValid());
    QTemporaryDir sendDir;
    QVERIFY(sendDir.isValid());

    const QString src = QDir(sendDir.path()).absoluteFilePath(QStringLiteral("回传.txt"));
    {
        QFile file(src);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("host-to-client"));
        file.close();
    }

    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    // 控制端那侧的接收器（生产里在 ClientConnection 内，这里直接用同一个类）
    lanrdp::FileReceiver receiver(recvDir.path());
    QStringList landed;
    QObject::connect(&receiver, &lanrdp::FileReceiver::finished, &receiver,
                     [&landed](bool ok, const QString &path, const QString &) {
                         if (ok)
                             landed.append(path);
                     });
    QObject::connect(&client, &lanrdp::Session::frameReceived, &receiver,
                     [&receiver, &client](const lanrdp::Frame &frame) {
                         receiver.onFrame(&client, frame);
                     });

    // ⚠️ QSignalSpy 而不是「lambda 捕获局部的 QStringList」：`logs` 声明在 `hc`
    // 之后，会先于它析构，而 `~HostController` → `stop()` 还会 emit 日志。
    QSignalSpy logs(&hc, &lanrdp::HostController::logLine);

    // ① 还没人持操作权
    QVERIFY(!hc.hasInputHolder());
    QVERIFY2(!hc.sendClipboardFilesToHolder({src}), "没有人持操作权时不该发");
    QVERIFY2(!logLines(logs).filter(QStringLiteral("没有控制端在操作本机")).isEmpty(),
             qPrintable(logLines(logs).join(QLatin1Char('\n'))));
    QTest::qWait(300);
    QVERIFY2(landed.isEmpty(), "被拒绝时不该有任何文件落到对端");

    // ② 拿到操作权
    lanrdp::Frame req;
    req.type = static_cast<std::uint8_t>(lanrdp::FrameType::TokenReq);
    client.sendFrame(req);
    QTRY_VERIFY(hc.hasInputHolder());

    QVERIFY(hc.sendClipboardFilesToHolder({src}));
    QTRY_COMPARE_WITH_TIMEOUT(landed.size(), 1, 15000);

    QCOMPARE(QFileInfo(landed.first()).fileName(), QStringLiteral("回传.txt"));
    QFile back(landed.first());
    QVERIFY(back.open(QIODevice::ReadOnly));
    QCOMPARE(back.readAll(), QByteArrayLiteral("host-to-client"));
}

// 【被控 → 发送文件…】：机主自己挑的文件，同一个收件人、同一道门，
// 但**不带** `CLIPBOARD_FILES` 清单 —— 那一帧的意思是「凑齐了写进对方的剪贴板」，
// 手挑的文件只是文件，多带一帧就等于替对方改剪贴板。
void TestHostController::pickedFilesGoToTheTokenHolder()
{
    QTemporaryDir recvDir;
    QVERIFY(recvDir.isValid());
    QTemporaryDir sendDir;
    QVERIFY(sendDir.isValid());

    const QString src = QDir(sendDir.path()).absoluteFilePath(QStringLiteral("手挑.txt"));
    {
        QFile file(src);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("picked-by-owner"));
        file.close();
    }

    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    lanrdp::FileReceiver receiver(recvDir.path());
    QStringList landed;
    QList<int> types;
    QObject::connect(&receiver, &lanrdp::FileReceiver::finished, &receiver,
                     [&landed](bool ok, const QString &path, const QString &) {
                         if (ok)
                             landed.append(path);
                     });
    QObject::connect(&client, &lanrdp::Session::frameReceived, &receiver,
                     [&receiver, &client, &types](const lanrdp::Frame &frame) {
                         types.append(int(frame.type));
                         receiver.onFrame(&client, frame);
                     });

    // 同上：必须是 QSignalSpy，不能捕获局部的 QStringList。
    QSignalSpy logs(&hc, &lanrdp::HostController::logLine);

    // ① 还没人持操作权：发不出去，也不该有半个字节上线
    QVERIFY(!hc.hasInputHolder());
    QVERIFY2(!hc.sendFilesToHolder({src}), "没有人持操作权时不该发");
    QVERIFY2(!logLines(logs).filter(QStringLiteral("没有控制端在操作本机")).isEmpty(),
             qPrintable(logLines(logs).join(QLatin1Char('\n'))));
    QTest::qWait(300);
    QVERIFY2(landed.isEmpty(), "被拒绝时不该有任何文件落到对端");
    QVERIFY2(!types.contains(int(lanrdp::FrameType::FileHeader)),
             "被拒绝时不该发 FILE_HEADER");

    // ② 拿到操作权
    lanrdp::Frame req;
    req.type = static_cast<std::uint8_t>(lanrdp::FrameType::TokenReq);
    client.sendFrame(req);
    QTRY_VERIFY(hc.hasInputHolder());

    QVERIFY(hc.sendFilesToHolder({src}));
    QTRY_COMPARE_WITH_TIMEOUT(landed.size(), 1, 15000);

    QCOMPARE(QFileInfo(landed.first()).fileName(), QStringLiteral("手挑.txt"));
    QFile back(landed.first());
    QVERIFY(back.open(QIODevice::ReadOnly));
    QCOMPARE(back.readAll(), QByteArrayLiteral("picked-by-owner"));

    QVERIFY2(!types.contains(int(lanrdp::FrameType::ClipboardFiles)),
             "手挑的文件不该发剪贴板清单（那会让对端把它们写进自己的剪贴板）");
}

// 隐私遮挡的宿主侧契约。
// 「暂停期间真的没发帧」由 test_streamer::pauseStopsFramesAndResumeForcesKeyframe 守；
// 这里守的是另一半：**锁**。而且必须跨 stop()/start() 保持 —— 停监听会清锁，
// 若不在重新监听时补回去，就会留下「本机黑着屏、新连进来的人却能操作」这种状态。
void TestHostController::privacyCurtainLocksInputAndSurvivesRestart()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("pw")));

    QVERIFY(!hc.privacyCurtain());
    QVERIFY(!hc.inputLocked());

    hc.setPrivacyCurtain(true);
    QVERIFY(hc.privacyCurtain());
    QVERIFY2(hc.inputLocked(), "遮挡期间必须锁住键鼠：黑着屏还能被操作是最坏的状态");

    // 幂等：热键与菜单同一拍触发时不该翻回去
    hc.setPrivacyCurtain(true);
    QVERIFY(hc.privacyCurtain());
    QVERIFY(hc.inputLocked());

    hc.stop();
    QVERIFY2(!hc.inputLocked(), "停监听本就会清锁（既有语义，这段不改它）");
    QVERIFY(hc.start(0, QStringLiteral("pw")));
    QVERIFY2(hc.inputLocked(), "遮挡没解除就重新监听，锁必须补回来");

    hc.setPrivacyCurtain(false);
    QVERIFY(!hc.privacyCurtain());
    QVERIFY2(!hc.inputLocked(), "退出遮挡要把自己加的那把锁解开");
}

// 机主自己先锁了，再开遮挡、再关：锁必须留着 —— 遮挡只解自己加的那把。
void TestHostController::privacyCurtainKeepsOwnersOwnLock()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("pw")));

    hc.takeInput();
    QVERIFY(hc.inputLocked());

    hc.setPrivacyCurtain(true);
    QVERIFY(hc.inputLocked());

    hc.setPrivacyCurtain(false);
    QVERIFY2(hc.inputLocked(), "机主自己锁的那把不能被遮挡顺手解开");
}

// 遮挡必须连声音一起停 —— 只停画面等于「捂住眼睛没捂住嘴」：
// 对方听得到声音，照样能推断机主在干什么。
//
// 音频走的是与画面**独立**的一条线（AudioCapturer → AudioStreamer），
// `ScreenStreamer::setPaused` 一点也管不到它，所以必须在这里显式停。
void TestHostController::privacyCurtainAlsoSilencesAudio()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    // 第一个控制端接入才起采集。没有音频输出设备的机器上它必然起不来 —— 那是环境事实。
    QElapsedTimer up;
    up.start();
    while (!hc.audioCaptureRunning() && up.elapsed() < 2000)
        QTest::qWait(50);
    const bool audioAvailable = hc.audioCaptureRunning();

    // ① 遮挡期间一块都不该采 —— 这条在任何环境下都要成立
    hc.setPrivacyCurtain(true);
    QVERIFY2(!hc.audioCaptureRunning(),
             "遮挡期间还在采集声音：屏幕黑了，声音照样送给对方");

    hc.setPrivacyCurtain(false);

    if (!audioAvailable)
        QSKIP("本机没有可用的音频输出设备（只验到「遮挡必须停采集」这一半）");

    // ② 撤销后要自己回来 —— 不能等下一个控制端接入才恢复（那时可能一直没人来）
    QElapsedTimer back;
    back.start();
    while (!hc.audioCaptureRunning() && back.elapsed() < 2000)
        QTest::qWait(50);
    QVERIFY2(hc.audioCaptureRunning(), "撤销遮挡后声音没回来");
}

// 遮挡期间**新连进来的人**不能把采集重新拉起来。
//
// 坑在 handleAuthenticated：「第一个音频目标接入就 startCapture」是无条件的，
// 遮挡期间它会把刚停下的设备又开起来 —— 现象是「盖着黑布却开始送声音」，
// 而且日志里只会看到一句「控制端已通过鉴权」，很难联想到音频。
void TestHostController::privacyCurtainKeepsNewClientFromStartingAudio()
{
    lanrdp::HostController hc;
    QVERIFY(hc.start(0, QStringLiteral("s3cret")));

    // 先确认真能起采集 —— 否则这条在没有声卡的机器上永远是假绿
    {
        auto *probe = new QTcpSocket();
        probe->connectToHost(QHostAddress::LocalHost, hc.port());
        QVERIFY(probe->waitForConnected(3000));

        lanrdp::Session client(probe);
        lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
        auth.start();
        QTRY_COMPARE(hc.clientCount(), 1);

        QElapsedTimer up;
        up.start();
        while (!hc.audioCaptureRunning() && up.elapsed() < 2000)
            QTest::qWait(50);
        if (!hc.audioCaptureRunning())
            QSKIP("本机没有可用的音频输出设备（这条测不出东西）");

        // 最后一个听的人走了，采集自己停（既有语义，这里只用来把状态清零）
        probe->disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(hc.clientCount(), 0, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!hc.audioCaptureRunning(), 3000);
    }

    hc.setPrivacyCurtain(true);
    QVERIFY2(!hc.audioCaptureRunning(), "遮挡开始时采集就该停了");

    // 现在接入的这个人 firstAudioTarget 为真，是最容易把设备重新拉起来的那条路
    auto *socket = new QTcpSocket();
    socket->connectToHost(QHostAddress::LocalHost, hc.port());
    QVERIFY(socket->waitForConnected(3000));

    lanrdp::Session client(socket);
    lanrdp::AuthClient auth(&client, QStringLiteral("s3cret"));
    auth.start();
    QTRY_COMPARE(hc.clientCount(), 1);

    // 起线程 + CoInitialize + 打开设备约几百毫秒，给足时间再判
    QTest::qWait(800);
    QVERIFY2(!hc.audioCaptureRunning(),
             "遮挡期间新连进来的控制端把声音采集又拉起来了：黑着屏反而开始送声音");
}

QTEST_GUILESS_MAIN(TestHostController)
#include "test_hostcontroller.moc"
