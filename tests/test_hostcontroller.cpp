#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
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
#include "net/Auth.h"
#include "net/Session.h"
#include "protocol/Frame.h"

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

// 锁屏 / 切换用户时，输入桌面不是 "Default" —— 此时 DXGI 采不到用户画面，
// 测出来的延迟与代码无关。
bool inputDesktopIsInteractable()
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
    return ok && ::lstrcmpiW(name, L"Default") == 0;
#else
    return true;
#endif
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
    void clientDisconnectRemovesItFromList();

    void tokenRequestIsGrantedToFirstClient();
    void takeInputLocksAndDropsHolder();
    void unlockInputAllowsRequestsAgain();
    void stopClearsClientsAndLock();

    void receiveDirRoundTrips();
    void codecChoiceIsRemembered();
    void statsStartAtZero();

    // 线程亲和性的「单一决定点」：内部定时器只能在 start() 里建。
    void timersExistOnlyAfterStart();

    // ---- 线程契约（宿主被搬到工作线程之后）----
    void movedHostBuildsTimersOnWorkerThread();
    void previewIsGatedOnHostSide();
    void startStopRepeatedlyDoesNotCrash();
    void hostStaysResponsiveWhileStreaming();
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
    if (!inputDesktopIsInteractable())
        QSKIP("当前会话不可交互（锁屏 / 切换用户），采不到用户画面");

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
    if (!inputDesktopIsInteractable())
        QSKIP("当前会话不可交互（锁屏 / 切换用户），采不到用户画面");

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

    QVERIFY2(rtts.size() >= 20, qPrintable(QStringLiteral("样本不足（%1/25）").arg(rtts.size())));
    std::sort(rtts.begin(), rtts.end());
    const qint64 med = rtts.at(rtts.size() / 2);
    const qint64 max = rtts.last();
    qInfo("host responsiveness while streaming: n=%d med=%lldms max=%lldms",
          static_cast<int>(rtts.size()), static_cast<long long>(med), static_cast<long long>(max));

    // 桌面面积超出判据适用范围时跳过：这里的数字反映分辨率，不反映退化。
    const QSize desk = desktopSize();
    if (qint64(desk.width()) * desk.height() > kMaxMeasuredPixels) {
        QSKIP(qPrintable(QStringLiteral(
            "桌面 %1×%2 上单帧采集+编码成本本身就有几百毫秒，绝对毫秒判据不适用"
            "（实测 med=%3ms max=%4ms，仅供观察）")
            .arg(desk.width()).arg(desk.height()).arg(med).arg(max)));
    }

    QVERIFY2(max < 500, qPrintable(QStringLiteral("推流期间被控端响应退化到 %1 ms").arg(max)));
}

QTEST_GUILESS_MAIN(TestHostController)
#include "test_hostcontroller.moc"
