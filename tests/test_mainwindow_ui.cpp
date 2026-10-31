#include <QtTest/QtTest>

#include <QAbstractButton>
#include <QAction>
#include <QDir>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStatusBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>

#include <limits>

#include "app/Dialogs.h"
#include "app/MainWindow.h"
#include "app/SessionStore.h"
#include "app/Theme.h"
#include "net/Auth.h"
#include "net/ClientConnection.h"
#include "net/HostServer.h"
#include "net/Session.h"
#include "protocol/FileTransfer.h"
#include "protocol/Frame.h"
#include "protocol/ScreenControl.h"
#include "protocol/ScreenFrame.h"
#include "viewer/EmptyState.h"
#include "viewer/RemoteView.h"
#include "viewer/ScreenOverlayBar.h"
#include "viewer/ScreenRenderer.h"
#include "viewer/ViewGrid.h"

// 界面结构 —— 画面区 + 顶部悬浮操作条 + 状态栏；三个对话框。
// 左列停靠面板连同它的两个滚动区、角色页签和 12/12b/12c 三轮布局微调一起删掉了，
// 所以那批「dock 几何 / 页签归属」用例也一并消失（不是改坏，是测的东西没了）。
// 需要 QApplication，故用 QTEST_MAIN；平台插件由 CMake 注入 QT_QPA_PLATFORM=offscreen。

// 每个 MainWindow 实例一份独立日志文件。
// 不给路径的话所有用例都往 <build>/lanrdp.log 追加 —— 几十个实例共享一个文件，
// 既会互相干扰（轮转把别家日志搬走），又让用例带上「跑过几遍」这种无关状态。
// 静态 QTemporaryDir 在进程退出时自动连带删掉目录里的一切。
QString testLogPath()
{
    static QTemporaryDir dir;
    static int           counter = 0;
    return dir.path() + QStringLiteral("/ui-%1.log").arg(++counter);
}

class TestMainWindowUi : public QObject
{
    Q_OBJECT

private slots:
    // 悬浮操作条
    void overlayBarGeometryIsPure();
    void overlayBarSitsOnTopOfScreen();
    void overlayBarButtonsFitInBar();
    void emptyStateGivesBothEntrances();
    // 空格的出口：多余的空格不想要了，条上【关闭】能关掉（唯一的空格不给出口）
    void emptyCellCanBeClosed();
    // 两个入口各自带一个矢量图标（不是两条光秃秃的文字按钮）
    void emptyStateButtonsCarryIcons();
    // 条上的纯图标按钮（占满 / 操作权 / 发送文件 / 本机预览）：不能退回文字、
    // 也不能只剩一个空图标 —— 这几颗按钮不带文字，提示是唯一的说明
    void overlayIconButtonsAreIconOnly();
    void overlayBarHasNoMenu();
    void overlayBarDoesNotForwardInputToRemote();
    void viewMenuTogglesOverlayBar();
    void statusBarIsIndependentOfOverlayBar();
    // 【设置】里的音频开关：可勾选、默认开、切换不崩（此刻没有任何连接 ⇒ 不该碰设备）
    void settingsMenuHasAudioSwitch();

    // 多屏：单屏用户界面零变化；多屏时条上多一颗【屏幕】，菜单项与屏列表一致
    void screenButtonAppearsOnlyWithMultipleScreens();
    void screenMenuListsEveryMonitorAndMarksCurrent();

    // 三个对话框
    void dialogsRoundTripTheirConfig();
    void dialogsAreDarkToo();
    void hostDialogExplainsFirewallPrompt();
    // 接收目录必须有默认值（空串不能原样显示成空框）
    void hostDialogPrefillsReceiveDir();
    // 口令框的「显示 / 隐藏」开关
    void passwordRevealToggleSwitchesEchoMode();
    void hostDialogRejectsWeakPassword();
    void connectDialogRejectsWeakPassword();
    // 地址为空与口令不合规是同一类事：提示必须是同一个位置的同一条红字
    void connectDialogRejectsEmptyAddressWithTheSameRedLabel();
    // 空态大图标要跟随屏幕 DPR 重画（跨缩放比屏拖动后不发糊）
    void emptyStateIconFollowsDevicePixelRatio();
    void startHostRejectsWeakPassword();
    void settingsDialogHoldsTheGlobalSwitches();
    // 被控端口令不得为空
    void hostDialogRejectsEmptyPassword();
    // 控制端地址不得为空
    void connectDialogRejectsEmptyHost();
    // 上次用过的会话参数要在窗口建好时就已经生效
    void restoresSavedSessionParameters();
    // 不传配置文件路径时，窗口不能去读真实配置
    void ignoresSessionStoreWhenPathIsEmpty();

    // 状态栏差分基线必须绑定「统计来源」
    void statusBaselineResetsWhenSwitchingCells();

    // 基本界面结构
    void clearLogEmptiesBufferOnly();          // 清日志 = 清内存缓冲
    void disconnectButtonStartsDisabled();
    void hostButtonTogglesListening();
    void disconnectButtonFollowsConnectionState();
    void transferProgressBarsStartHidden();

    // 改端口重新开始监听：调用方契约 —— 必须真的换到新端口并释放旧端口
    void startHostOnNewPortRebinds();

    // 被控端收回输入权（双态图标按钮 + 锁定语义）
    void takeInputButtonStartsReadyToTake();
    void takeInputButtonTogglesLockState();
    void stopHostClearsInputLock();
    void endToEndTakeInputLocksOutClient();
    // 控制端的操作权状态图标（没有文字，三态只能靠图形区分）
    void tokenStatusIconFollowsToken();

    // 文件传输与剪贴板同级受输入令牌门控（P26）：无令牌传不了，令牌交还后即可传
    void fileTransferFollowsInputToken();
    // 令牌身份与裸 Session* 解耦：断开后令牌必须空出，新会话要自己申请才能拿到
    void disconnectedHolderDoesNotBlockNextSession();
    // --send-file 的收尾契约：等写缓冲排空 + 半关，大文件才能真正送到对端
    void sendFileCliShutdownDeliversLargeFile();

    // 断线自动重连
    void autoReconnectDefaultsOn();
    void failedConnectSchedulesReconnect();
    void userDisconnectCancelsPendingReconnect();
    void uncheckedAutoReconnectDoesNotSchedule();

    // 手工验证时发现的瑕疵：重连待命时点【断开】，界面没跟着复位
    void userDisconnectWhileReconnectPendingResetsUi();

    // 跨机验收（2026-09-21）暴露的：输入被锁定后，控制端一按键就刷出上万行拒绝日志
    void lockedInputDoesNotFloodTokenRequests();

    // 被控 = 全局功能（【被控】菜单 + 状态栏），与控制格并存、互不抢占窗口
    void modeMenuStartsUnselected();
    void modeActionSwitchesOverlayBar();
    void hostingLocksModeEntry();
    void clientConnectLocksModeEntry();
    void lockedModeHidesOtherModeActions();

    // 一控多（每格一条连接 + 选中路由）
    void secondConnectOpensSecondCell();
    void connectCellSelectionFollowsGrid();
    void clickingACellSelectsIt();
    void viewCountIsCappedAtFour();
    void hostModeCollapsesExtraCells();

    // 占满 / 还原
    void maximizeButtonNeedsASecondCell();
    void maximizeFillsItsOwnCellAndRestores();
    void escapeRestoresFromMaximized();
    void barDoubleClickTogglesMaximize();
    void maximizeMovesFocusSoEscapeWorks();
    void barButtonsNeverTakeFocus();
    void singleCellUsesQualityScaling();
    void addingACellLeavesMaximizedState();

    // 断线格的表现
    void disconnectedCellKeepsItsLastFrame();
    void retryButtonReconnectsThatCell();
    void closeButtonDropsOnlyThatCell();
    void closeButtonKeepsSelectionOnTheUntouchedCell();
    void closingTheOnlyCellResetsIt();
    void closingACellExpandsTheRemainingOne();
    void disconnectedCellOnlyOffersTheWayOut();

    // 验收时抓到的真 bug —— 画面没铺满自己的格子
    void rendererFillsItsCell();
    void rendererKeepsFillingAfterRelayout();

    // 输入路径加固：
    // · 未启用键鼠时要有**可见提示**，点它即开启（否则输入被静默丢弃）
    void disabledInputShowsClickableHint();
    void clickingHintEnablesInput();
    // · 「按住键 → 关开关 → 抬起仍要发出」完整链条（远端卡键）
    void turningOffSwitchReleasesPressedKey();

    // 心跳只能单向（定时器）驱动 —— 被控端不能对收到的 Heartbeat 回帧
    void hostDoesNotEchoHeartbeat();
};

namespace {

// 找一个「刚刚空着、当前没人监听」的端口：先让系统分一个再关掉。
// 直接用一个固定端口不行 —— 那台机器上可能真的开着服务，测的就不是「连不上」了。
quint16 pickDeadPort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return 0;
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

// 条上控件统一按 QAbstractButton 找 —— 它们是 QToolButton
//（条高只有 30 逻辑像素，QPushButton 的 min-height 会撑出条外）。
// 用基类而非具体类，是为了「换个按钮类型不必改测试」。
QAbstractButton *barButton(MainWindow &w, const char *name)
{
    return w.findChild<QAbstractButton *>(QString::fromLatin1(name));
}

// 条上控件的显隐要用 isHidden() 读：它只看「有没有被显式 hide 过」，
// 不受祖先可见性影响。而 isVisible() 在 offscreen + 未 show() 时对谁都是 false，
// 读出来什么也证明不了。
bool shown(QAbstractButton *b)
{
    return b && !b->isHidden();
}

// 伪随机内容 —— 别用全 0，否则「整份收到」和「只收到一小段」在 sha256 上不好区分。
QByteArray makeBytes(int size)
{
    QByteArray data;
    data.resize(size);
    quint32 state = 0xC0FFEEu;
    for (int i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        data[i] = static_cast<char>((state >> 11) & 0xFF);
    }
    return data;
}

QString writeFileInto(const QString &dir, const QString &name, const QByteArray &content)
{
    const QString path = QDir(dir).absoluteFilePath(name);
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    file.close();
    return path;
}

QByteArray readWholeFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

// ---------------------------------------------------------------------------
// 悬浮操作条
// ---------------------------------------------------------------------------

// 纯函数：贴顶、满宽、高度固定。抽出来就是为了不起真窗口也能钉住这条约束。
void TestMainWindowUi::overlayBarGeometryIsPure()
{
    const int h = lanrdp::ScreenOverlayBar::barHeight();
    QVERIFY2(h > 0, "条高必须为正");

    QCOMPARE(lanrdp::ScreenOverlayBar::geometryFor(QSize(800, 600)), QRect(0, 0, 800, h));
    QCOMPARE(lanrdp::ScreenOverlayBar::geometryFor(QSize(1920, 1080)), QRect(0, 0, 1920, h));

    // 尺寸还没落定（宽 0）时不能算出负宽 —— 那会让 setGeometry 抛警告
    QCOMPARE(lanrdp::ScreenOverlayBar::geometryFor(QSize(0, 0)).width(), 0);
}

// 条必须是**画面控件的子控件**，且贴顶满宽。
// 这两条合起来才是「本地手势不会漏到远端」的全部依据（见下一条用例）。
void TestMainWindowUi::overlayBarSitsOnTopOfScreen()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTest::qWait(100);

    auto *bar      = w.overlayBar();
    auto *renderer = w.screenRenderer();
    QVERIFY2(bar, "应当存在悬浮操作条");
    QVERIFY2(renderer, "应当存在画面控件");

    // 中央部件**不再是画面控件本身** —— 一控多要在中央区放 1..4 个视窗，
    // 而 `setCentralWidget()` 只能有一个，所以换成 ViewGrid 承载，画面降为第 0 格。
    // 格子里装的东西也从「裸画面控件」升格为**一整格**（RemoteView =
    // 画面 + 条 + 条上控件 + 该格连接）—— 一控多要的是「一整套」，不是光一块画布。
    auto *grid = w.viewGrid();
    QVERIFY2(grid, "中央区应当是多视窗网格宿主");
    QCOMPARE(w.centralWidget(), static_cast<QWidget *>(grid));
    QCOMPARE(grid->viewCount(), 1);

    auto *view = qobject_cast<lanrdp::RemoteView *>(grid->viewAt(0));
    QVERIFY2(view, "第 0 格应当是一格 RemoteView");
    QCOMPARE(view, w.selectedView());
    QCOMPARE(view->renderer(), renderer);
    QCOMPARE(renderer->parentWidget(), static_cast<QWidget *>(view));

    // ⚠️ 这是整条设计的地基：条是画面的子控件 ⇒ Qt 的命中测试会把落在条上的
    // 事件交给条，画面控件的 mousePressEvent / keyPressEvent 根本不会被调用。
    // （画面区的右键已经属于远端、双击的第二下会被转手再调一次 mousePressEvent、
    // Esc 也会被无条件转发 —— 所以本地手势只能落在事件到不了画面的地方。）
    QCOMPARE(bar->parentWidget(), renderer);

    QCOMPARE(bar->geometry().top(), 0);
    QCOMPARE(bar->width(), renderer->width());
    QCOMPARE(bar->height(), lanrdp::ScreenOverlayBar::barHeight());

    // 不能被设成「鼠标穿透」—— 那等于把条上的点击又让回给画面控件了
    QVERIFY2(!bar->testAttribute(Qt::WA_TransparentForMouseEvents),
             "悬浮条不能鼠标穿透，否则条上的点击会落到画面控件上");
}

// 条是**定高 30** 的，按钮一旦比它高就被裁掉下半截 —— 文字只剩上半截，
// 看起来像字体坏了，而不是像布局问题。
//
// ⚠️ 这条必须在**真实主题**下跑。QTEST_MAIN 默认用平台原生样式，而原生
// QToolButton 比 Fusion 矮，量不出这个问题；真机上换样式后才露出来。
void TestMainWindowUi::overlayBarButtonsFitInBar()
{
    lanrdp::theme::apply(*qApp);

    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTest::qWait(50);

    auto *bar = w.overlayBar();
    QVERIFY2(bar, "应当存在悬浮操作条");

    const int barH = lanrdp::ScreenOverlayBar::barHeight();
    QCOMPARE(bar->height(), barH);

    const auto buttons = bar->findChildren<QAbstractButton *>();
    QVERIFY2(!buttons.isEmpty(), "条上应当有按钮");

    int visible = 0;
    // 条上大半是纯图标按钮，没有文字 —— 报错时退回 objectName，否则
    // 失败消息里只剩一对空引号，看不出是哪颗按钮顶出了条。
    const auto label = [](QAbstractButton *b) {
        return b->text().isEmpty() ? b->objectName() : b->text();
    };

    // 空格的条上**允许一个按钮都看不见** —— 开场入口已经搬到空态层了，
    // 条只在「这一格有连接」时才长出操作组。所以几何检查只量此刻可见的，
    // 「至少露一颗」放到连接建立之后断言。
    for (QAbstractButton *b : buttons) {
        if (!b->isVisible())
            continue;   // 没连接的格上按钮本来就收起了，量它没有意义
        ++visible;

        QVERIFY2(b->height() <= barH,
                 qPrintable(QStringLiteral("按钮「%1」高 %2 px，超出条高 %3 px")
                                .arg(label(b)).arg(b->height()).arg(barH)));
        QVERIFY2(b->geometry().top() >= 0 && b->geometry().bottom() < barH,
                 qPrintable(QStringLiteral("按钮「%1」的纵向范围 %2..%3 出了条（0..%4）")
                                .arg(label(b)).arg(b->geometry().top())
                                .arg(b->geometry().bottom()).arg(barH - 1)));
    }

    // 连上一台远端：条上出现控制端操作组，这时「塞得进 30px」才算真正被量过。
    lanrdp::HostServer peer(QStringLiteral("secret"));
    QVERIFY(peer.listen(0));
    w.startConnect(QStringLiteral("127.0.0.1"), peer.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    QTest::qWait(50);

    visible = 0;
    for (QAbstractButton *b : buttons) {
        if (!b->isVisible())
            continue;
        ++visible;

        QVERIFY2(b->height() <= barH,
                 qPrintable(QStringLiteral("按钮「%1」高 %2 px，超出条高 %3 px")
                                .arg(label(b)).arg(b->height()).arg(barH)));
        QVERIFY2(b->geometry().top() >= 0 && b->geometry().bottom() < barH,
                 qPrintable(QStringLiteral("按钮「%1」的纵向范围 %2..%3 出了条（0..%4）")
                                .arg(label(b)).arg(b->geometry().top())
                                .arg(b->geometry().bottom()).arg(barH - 1)));
    }
    QVERIFY2(visible > 0, "连接之后条上应当至少露出一颗操作按钮");
}

// 未选模式的空态不能只是一行字 —— 它得给两个真入口，而且只在「未选」时出现。
//
// 这条守的是可用性：从前打开软件第一屏写着「从【模式】菜单选…」，
// 等于把第一步交给用户去菜单里找。
void TestMainWindowUi::emptyStateGivesBothEntrances()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTest::qWait(50);

    auto *view = w.viewAt(0);
    QVERIFY(view);
    auto *es = view->emptyState();
    QVERIFY2(es, "每一格都应当有空态层");

    // 未选模式：出现，并且铺满画面（它靠 eventFilter 跟随画面尺寸）
    QVERIFY2(es->isVisible(), "未选模式应当显示空态层");
    QCOMPARE(es->size(), view->renderer()->size());

    auto *primary   = es->findChild<QPushButton *>(QStringLiteral("emptyPrimary"));
    auto *secondary = es->findChild<QPushButton *>(QStringLiteral("emptySecondary"));
    QVERIFY2(primary && primary->isVisible(), "应当有【连接对方电脑】");
    QVERIFY2(secondary && secondary->isVisible(), "应当有【让对方连进来】");

    // ⚠️ 空态层盖在画面控件上，而画面控件会把**未处理**的键鼠事件转发给远端。
    // 它必须自己把事件吃掉 —— 与悬浮条同一个理由。
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(30, 30), QPointF(30, 30),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    press.setAccepted(false);
    QCoreApplication::sendEvent(es, &press);
    QVERIFY2(press.isAccepted(), "空态层必须把落在它身上的鼠标事件吃掉（否则会冒泡给画面）");

    // 开始监听：空态**仍然显示** —— 被控是全局功能，不改变任何一格的状态，
    // 这个空格还是没有连接，摆两个入口依然成立。但次入口要随状态换双态：
    // 监听中显示「禁止对方接入」，点它 = 停止监听（不是再弹一次对话框）。
    w.startHost(0, QStringLiteral("secret"));   // 阻塞投递，返回即已生效
    QVERIFY2(w.isHosting(), "应当正在监听");
    QVERIFY2(es->isVisible(), "监听中空格依旧没有连接，空态层不该消失");
    QCOMPARE(secondary->text(), QStringLiteral("禁止对方接入"));

    // 点「禁止对方接入」→ 停止监听，按钮翻回「允许对方接入」。
    secondary->click();
    QVERIFY2(!w.isHosting(), "监听中点【禁止对方接入】应当停止监听");
    QCOMPARE(secondary->text(), QStringLiteral("允许对方接入"));

    // 重新开始监听后再连出去（填上对端）：这一格不再空，空态层退场。
    // 用连不上的端口也行 —— 连接对象一旦存在，空态就归画面与断线出口管。
    w.startHost(0, QStringLiteral("secret"));
    QVERIFY(w.isHosting());
    w.startConnect(QStringLiteral("127.0.0.1"), pickDeadPort(), QStringLiteral("secret"));
    QVERIFY2(!es->isVisible(), "有连接对象的格不该显示空态层");
}

// 空格的出口：新增的空格不想要了，应当能从条上【关闭】关掉。
// 唯一的空格不给出口 —— 中央区总得留一格，关了它等于关掉软件的入口。
void TestMainWindowUi::emptyCellCanBeClosed()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    // 唯一的空格：没有【关闭】出口。
    QVERIFY2(!shown(w.viewAt(0)->closeCellButton()),
             "唯一的空格不该显示【关闭】");

    // 条上【连接另一台】= 加一个空格：新空格条上出现【关闭】。
    w.viewAt(0)->connectButton()->click();
    QCOMPARE(w.viewCount(), 2);
    QVERIFY2(shown(w.viewAt(1)->closeCellButton()), "多格时空格应当有【关闭】出口");
    QVERIFY2(!shown(w.viewAt(1)->retryButton()), "空格不是断线格，不该有【重连】");

    // 点掉它：回到一格，剩下的仍是空态。
    w.viewAt(1)->closeCellButton()->click();
    QCOMPARE(w.viewCount(), 1);
    QVERIFY2(!w.viewAt(0)->hasConnection(), "关闭新格不应影响原有空格");
    QVERIFY2(w.viewAt(0)->emptyState()->isVisible(), "剩余空格应当显示空态层");
}


// 空态的两个入口各自带一个矢量图标。
//
// 守两件事：① 图标没被漏设（空 QIcon 也是「非 null」，得看像素）；
// ② 图标画得出东西 —— 路径写错时 QIcon 照样返回一张全透明的图，不报错。
void TestMainWindowUi::emptyStateButtonsCarryIcons()
{
    MainWindow w(testLogPath());

    auto *view = w.viewAt(0);
    QVERIFY(view);
    auto *es = view->emptyState();
    QVERIFY(es);

    auto *primary   = es->findChild<QPushButton *>(QStringLiteral("emptyPrimary"));
    auto *secondary = es->findChild<QPushButton *>(QStringLiteral("emptySecondary"));
    QVERIFY(primary && secondary);

    for (QPushButton *b : { primary, secondary }) {
        QVERIFY2(!b->iconSize().isEmpty(),
                 qPrintable(QStringLiteral("「%1」的图标尺寸是 0").arg(b->text())));

        const QPixmap pm = b->icon().pixmap(b->iconSize());
        QVERIFY2(!pm.isNull(), qPrintable(QStringLiteral("「%1」的图标出了空图").arg(b->text())));

        bool painted = false;
        const QImage img = pm.toImage();
        for (int y = 0; y < img.height() && !painted; ++y) {
            for (int x = 0; x < img.width() && !painted; ++x)
                painted = qAlpha(img.pixel(x, y)) > 0;
        }
        QVERIFY2(painted, qPrintable(QStringLiteral("「%1」的图标是全透明的").arg(b->text())));
    }
}

// 条上的纯图标按钮：**不能有文字**，而且图标必须真的画出了东西。
//
// 这几颗按钮都没有文字，悬停提示是唯一的说明 —— 所以任何一颗漏了 setIcon、
// 或者尺寸被设成 0，用户看到的就是一个只剩 hover 反馈的空洞，界面上无从察觉。
void TestMainWindowUi::overlayIconButtonsAreIconOnly()
{
    MainWindow w(testLogPath());

    const char *names[] = { "maximizeButton", "tokenStatusButton",
                            "releaseTokenButton", "sendFileButton",
                            "disconnectButton", "retryButton",
                            "closeCellButton", "connectButton" };

    for (const char *name : names) {
        const QString tag = QString::fromLatin1(name);

        auto *b = qobject_cast<QToolButton *>(barButton(w, name));
        QVERIFY2(b, qPrintable(QStringLiteral("条上找不到 %1").arg(tag)));

        QVERIFY2(b->toolButtonStyle() == Qt::ToolButtonIconOnly,
                 qPrintable(QStringLiteral("%1 不是纯图标样式，文字会又冒出来").arg(tag)));
        QVERIFY2(b->text().isEmpty(),
                 qPrintable(QStringLiteral("%1 还留着文字「%2」").arg(tag, b->text())));

        QVERIFY2(!b->iconSize().isEmpty(),
                 qPrintable(QStringLiteral("%1 的图标尺寸被设成了 0").arg(tag)));
        QVERIFY2(!b->icon().isNull(),
                 qPrintable(QStringLiteral("%1 根本没有图标").arg(tag)));

        const QPixmap pm = b->icon().pixmap(b->iconSize());
        QVERIFY2(!pm.isNull(), qPrintable(QStringLiteral("%1 的图标出了空图").arg(tag)));

        bool painted = false;
        const QImage img = pm.toImage();
        for (int y = 0; y < img.height() && !painted; ++y) {
            for (int x = 0; x < img.width() && !painted; ++x)
                painted = qAlpha(img.pixel(x, y)) > 0;
        }
        QVERIFY2(painted, qPrintable(QStringLiteral("%1 的图标是全透明的").arg(tag)));

        // 没有文字时，无障碍名字与悬停提示是仅有的两条说明，缺一条这颗按钮
        // 对读屏用户就成了「按钮」两个字的空壳。
        QVERIFY2(!b->accessibleName().isEmpty(),
                 qPrintable(QStringLiteral("%1 没有无障碍名字").arg(tag)));
        QVERIFY2(!b->toolTip().isEmpty(),
                 qPrintable(QStringLiteral("%1 没有悬停提示").arg(tag)));
    }

    // 【已连 N 个】是唯一不是「纯图标」的一颗：人形 + 数字。它住在**悬浮条最左**
    // （未监听时隐藏），objectName 沿用旧名。数字必须是**数字** ——
    // 一旦有人把它写回「已连 3 个」，这颗按钮就又变回文字按钮了（宽度也是这一条在守）。
    auto *list = qobject_cast<QToolButton *>(barButton(w, "clientListButton"));
    QVERIFY2(list, "找不到 clientListButton（悬浮条）");
    QVERIFY2(list->toolButtonStyle() == Qt::ToolButtonTextBesideIcon,
             "「已连 N 个」应当是图标 + 数字，不是纯文字也不是纯图标");
    QVERIFY2(!list->icon().isNull(), "「已连 N 个」没有图标（只剩一个数字）");
    QVERIFY2(!list->text().isEmpty(), "「已连 N 个」连数字都没有");
    for (const QChar ch : list->text()) {
        QVERIFY2(ch.isDigit(),
                 qPrintable(QStringLiteral("「已连 N 个」的文字只能放数字，现在是「%1」")
                                .arg(list->text())));
    }
    QVERIFY2(list->accessibleName().contains(QStringLiteral("已连")),
             "「已连 N 个」的无障碍名字要说清这是已连几个控制端");
    QVERIFY2(!list->toolTip().isEmpty(), "「已连 N 个」没有悬停提示");
}

// 菜单只剩「复制对方地址」这种半成品入口 ⇒ 整个去掉（用户决定，2026-10-17）。
void TestMainWindowUi::overlayBarHasNoMenu()
{
    MainWindow w(testLogPath());

    auto *bar = w.overlayBar();
    QVERIFY(bar);
    QVERIFY2(!w.findChild<QToolButton *>(QStringLiteral("overlayMenuButton")),
             "悬浮条不该再有「⋮」按钮");
    QVERIFY2(w.viewAt(0)->findChildren<QMenu *>().isEmpty(),
             "这一格不该再挂菜单对象（右键也不该弹）");

    // 控制会话进行中也不该冒出来（这是它以前唯一有内容的时候）
    w.startConnect(QStringLiteral("127.0.0.1"), pickDeadPort(), QStringLiteral("secret"));
    QVERIFY2(!w.findChild<QToolButton *>(QStringLiteral("overlayMenuButton")),
             "控制模式下也不该有「⋮」按钮");
    QVERIFY2(w.viewAt(0)->findChildren<QMenu *>().isEmpty(),
             "控制模式下也不该挂菜单对象");
}

// 在条上乱点 / 乱按键，都不能变成发往远端的输入事件。
//
// ⚠️ 这条不是形式主义 —— 它第一版跑出来是**红的**：5 次模拟点击全部冒泡到了画面
// 控件。根因是 Qt 对**未处理**的鼠标 / 键盘事件会向上传播给父控件，而条的父控件
// 正是画面控件。所以「把条做成独立子控件」只是地基，「条显式吃掉事件」才是墙。
void TestMainWindowUi::overlayBarDoesNotForwardInputToRemote()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTest::qWait(100);

    auto *renderer = w.screenRenderer();
    auto *bar      = w.overlayBar();
    QVERIFY(renderer && bar);

    // 让画面区进入「采集输入」状态 —— 否则就算事件真的漏过去也不会发信号，测不出东西
    renderer->setInputEnabled(true);

    QSignalSpy buttons(renderer, &lanrdp::ScreenRenderer::mouseButtonChanged);
    QSignalSpy moved(renderer, &lanrdp::ScreenRenderer::mouseMoved);
    QSignalSpy keys(renderer, &lanrdp::ScreenRenderer::keyChanged);
    QSignalSpy dropped(renderer, &lanrdp::ScreenRenderer::fileDropped);

    // 条文空白处（标题右侧那一段）
    const QPoint blank(qMax(40, bar->width() / 2), bar->height() / 2);
    QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, blank);
    QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, blank);
    QTest::mousePress(bar, Qt::RightButton, Qt::NoModifier, blank);
    QTest::mouseRelease(bar, Qt::RightButton, Qt::NoModifier, blank);
    QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, blank);

    // Esc 是重点观察对象：它在 KeyMap 里被映射成 0x1B 且会被无条件转发，
    // 所以「Esc 能不能用来做本地手势」的答案是「不能」—— 条上按它也不该有反应。
    QTest::keyClick(bar, Qt::Key_Escape);
    QTest::keyClick(bar, Qt::Key_A);

    QCOMPARE(buttons.count(), 0);
    QCOMPARE(moved.count(), 0);
    QCOMPARE(keys.count(), 0);
    QCOMPARE(dropped.count(), 0);
}

// 条会遮住画面顶部约 30 逻辑像素（远端窗口的标题栏 / 最大化按钮可能
// 因此点不到），所以必须留一个收起入口。Ctrl+L 是早就有的肌肉记忆。
void TestMainWindowUi::settingsMenuHasAudioSwitch()
{
    MainWindow w(testLogPath());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto *audio = w.findChild<QAction *>(QStringLiteral("actionReceiveAudio"));
    QVERIFY2(audio, "【设置】菜单里应当有「接收对方的声音」");
    QVERIFY2(audio->isCheckable(), "开关项应当可勾选");
    QVERIFY2(audio->isChecked(),
             "默认应当接收声音 —— 默认关会变成「明明连着却听不见」这种无从解释的现象");
    QVERIFY2(!audio->statusTip().isEmpty(), "全局开关也应当有一句说明");

    // 切两次都不许崩：此刻没有任何连接，切换不该去碰声卡（碰了就会在没设备的机器上炸）
    audio->trigger();
    QVERIFY(!audio->isChecked());
    audio->trigger();
    QVERIFY(audio->isChecked());
}

void TestMainWindowUi::viewMenuTogglesOverlayBar()
{
    MainWindow w(testLogPath());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto *bar = w.overlayBar();
    QVERIFY(bar);

    auto *toggle = w.findChild<QAction *>(QStringLiteral("actionToggleOverlayBar"));
    QVERIFY2(toggle, "【视图】菜单里应当有悬浮条的开关项");
    QVERIFY2(toggle->isCheckable(), "开关项应当可勾选");
    QVERIFY2(toggle->isChecked(), "默认应当显示悬浮条");
    QVERIFY2(bar->isVisible(), "默认悬浮条应当可见");

    toggle->trigger();
    QVERIFY2(!bar->isVisible(), "取消勾选后悬浮条应当隐藏");
    QVERIFY2(w.centralWidget() != nullptr, "条隐藏后画面区必须还在");

    toggle->trigger();
    QVERIFY2(bar->isVisible(), "再勾选应当恢复显示");
}

// 状态栏是 statusBar() 的永久控件，独立于悬浮条 —— 收起条不该把 fps/Mbps 一起带走。
void TestMainWindowUi::statusBarIsIndependentOfOverlayBar()
{
    MainWindow w(testLogPath());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto *bar    = w.overlayBar();
    auto *status = w.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY(bar);
    QVERIFY2(status, "状态栏标签应当存在（objectName=statusLabel）");

    QVERIFY2(!bar->isAncestorOf(status),
             "状态栏标签不能挂在悬浮条下面，否则收起条就看不到 fps/Mbps 了");

    bar->hide();
    QVERIFY2(w.centralWidget() != nullptr, "条隐藏后画面区必须还在");
    QVERIFY2(status->isVisible(), "条隐藏后状态栏标签仍应可见");
}

// 状态栏的差分基线必须**绑定统计来源**。
//
// 一控多时「来源」= 选中格那条连接。换格后新来源的累计计数会**突然变小**
// （比如从 5 帧的记录切到 0 帧的那一格），拿上一格的基线去减就发生
// **无符号回绕** —— 状态栏会刷出 1.8e19 fps / 上亿 Mbps 这种天文数字。
void TestMainWindowUi::statusBaselineResetsWhenSwitchingCells()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());
    QCOMPARE(hostA.sessions().size(), 1);
    QCOMPARE(hostB.sessions().size(), 1);

    auto *status = w.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY2(status, "状态栏标签应当存在（objectName=statusLabel）");

    // 只读状态栏文案里的 fps 字段，不碰任何内部状态。
    // ⚠️ 必须用 toULongLong：回绕出来的 18446744073709551611 **超出 qint64 上限**，
    // toLongLong() 会静默返回 0 —— 恰好把这条缺陷藏起来（第一版就是这么写成假绿的）。
    auto fpsOf = [&]() -> qulonglong {
        const QRegularExpression re(QStringLiteral("\\| (\\d+) fps \\|"));
        const auto               m = re.match(status->text());
        if (!m.hasMatch())
            return std::numeric_limits<qulonglong>::max();
        bool              ok = false;
        const qulonglong  v  = m.captured(1).toULongLong(&ok);
        return ok ? v : std::numeric_limits<qulonglong>::max();
    };

    // 让第 0 格真的「收到」几帧：走裸帧路径（不需要编码器 / 解码线程）。
    const QByteArray raw = lanrdp::encodeRawFrame(QImage(16, 12, QImage::Format_ARGB32));
    QVERIFY(!raw.isEmpty());
    lanrdp::Session *peerA = hostA.sessions().at(0);
    QVERIFY(peerA);
    for (int i = 0; i < 4; ++i)
        peerA->sendFrame(static_cast<quint8>(lanrdp::FrameType::ScreenFrameRaw), raw);
    QTRY_COMPARE(w.viewAt(0)->connection()->framesReceived(), quint64(4));

    // 选中第 0 格并刷一拍：基线落在这条连接上（计数器 = 4）
    w.viewGrid()->setSelectedIndex(0);
    QMetaObject::invokeMethod(&w, "updateStats");

    // 再收一帧 —— 差分正常工作，状态栏应当报 1 fps（并稳定在 1）
    peerA->sendFrame(static_cast<quint8>(lanrdp::FrameType::ScreenFrameRaw), raw);
    QTRY_COMPARE(w.viewAt(0)->connection()->framesReceived(), quint64(5));
    QTRY_VERIFY2(fpsOf() == 1, qPrintable(QStringLiteral("状态栏文案：") + status->text()));

    // 关键一步：切到第 1 格（它一帧没收到）。基线若不随来源重置 -> 0 - 5 无符号回绕。
    w.viewGrid()->setSelectedIndex(1);
    QVERIFY(QMetaObject::invokeMethod(&w, "updateStats"));
    QVERIFY2(fpsOf() < 100,
             qPrintable(QStringLiteral("换格后状态栏出现无符号回绕：") + status->text()));
}

// ---------------------------------------------------------------------------
// 三个对话框
// ---------------------------------------------------------------------------

// 深色化不能只到主窗口为止。对话框是**独立顶层窗口**，QSS 里漏掉某个控件类
// 或某条背景规则，用户点开【开始监听…】迎面就是一块白底 —— 而主窗口看起来完全正常。
//
// 用 grab() 把对话框渲染成图像再量像素：不依赖真实屏幕（offscreen 下 grab 走
// QPainter 渲染，颜色与真机一致），也比截屏可靠。
void TestMainWindowUi::dialogsAreDarkToo()
{
    lanrdp::theme::apply(*qApp);

    lanrdp::HostConfigDialog host(lanrdp::HostConfigDialog::Config{}, nullptr);
    host.resize(460, 300);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));

    const QImage shot = host.grab().toImage();
    QVERIFY2(!shot.isNull(), "对话框应当能被渲染成图像");

    // 四个角各取一点：对话框底色铺满整块，任何一角发亮都说明背景规则没生效。
    const struct {
        const char *where;
        int         x;
        int         y;
    } corners[] = {
        {"左上", 4, 4},
        {"右上", shot.width() - 5, 4},
        {"左下", 4, shot.height() - 5},
        {"右下", shot.width() - 5, shot.height() - 5},
    };

    for (const auto &c : corners) {
        const QColor px = shot.pixelColor(c.x, c.y);
        QVERIFY2(px.lightness() < 60,
                 qPrintable(QStringLiteral("对话框%1角是 %2 —— 深色主题没有覆盖到它")
                                .arg(QString::fromUtf8(c.where), px.name())));
    }
}

// 防火墙只在某个端口**第一次**被监听时问一次：用户当时点了跳过，系统会生成
// 阻止规则且不再提示，而程序这边看不出任何异常（本机 listen 照常成功，只是外部
// 连不进来）。所以「连不上就去防火墙放行」这句话必须留在对话框里 —— 它是这个
// 故障唯一的事前防线（不做自动加规则：那要提权，而这辈子只点一两次）。
//
// 文案**不提网络类型**：弹窗里「专用 / 公用」两项勾哪个取决于用户的网卡被判成
// 哪一类，指错方向等于没放行。反向断言就是为此设的 —— 别把「专用网络」写回去。
void TestMainWindowUi::hostDialogExplainsFirewallPrompt()
{
    lanrdp::HostConfigDialog host(lanrdp::HostConfigDialog::Config{}, nullptr);

    QString hint;
    for (QLabel *l : host.findChildren<QLabel *>()) {
        if (l->text().contains(QStringLiteral("防火墙")))
            hint = l->text();
    }
    QVERIFY2(!hint.isEmpty(), "开始监听对话框里应当有防火墙提示");
    QVERIFY2(hint.contains(QStringLiteral("放行")),
             qPrintable(QStringLiteral("提示没说要放行：") + hint));
    QVERIFY2(!hint.contains(QStringLiteral("专用网络")),
             qPrintable(QStringLiteral("又去指网络类型了（网卡归属不定，指哪边都可能错）：") + hint));
}

// 接收目录不能显示成空框：真正落盘时会退到系统「下载」目录（MainWindow::receiveDir()），
// 框里却空着，等于告诉用户"这项没设" —— 首次运行（ini 里还没有这一项）就是这样。
void TestMainWindowUi::hostDialogPrefillsReceiveDir()
{
    // 反面：配置里带了具体目录，必须原样显示（别把默认值硬塞进去）。
    lanrdp::HostConfigDialog::Config given;
    given.receiveDir = QStringLiteral("D:/tmp/received");
    lanrdp::HostConfigDialog withValue(given, nullptr);
    auto *givenEdit = withValue.findChild<QLineEdit *>(QStringLiteral("recvDirEdit"));
    QVERIFY2(givenEdit, "应当有接收目录输入框（objectName=recvDirEdit）");
    QCOMPARE(givenEdit->text(), QStringLiteral("D:/tmp/received"));

    // 正面：传空串（没存过）时要退到默认目录，而不是留空。
    lanrdp::HostConfigDialog::Config empty;
    empty.receiveDir = QString();
    lanrdp::HostConfigDialog dlg(empty, nullptr);
    auto *edit = dlg.findChild<QLineEdit *>(QStringLiteral("recvDirEdit"));
    QVERIFY2(edit, "应当有接收目录输入框（objectName=recvDirEdit）");
    QVERIFY2(!edit->text().trimmed().isEmpty(), "接收目录不该是空框");
    QCOMPARE(edit->text(), lanrdp::defaultReceiveDir());
}

// 口令框右侧的开关：默认隐藏，点一次看明文、再点回隐藏。
// ⚠️ 切换**只该动 echoMode** —— 校验和落盘读的都是 text()，这里顺带把内容也断言一遍，
// 免得哪天为了"显示得好一点"去动输入框里的文字。
void TestMainWindowUi::passwordRevealToggleSwitchesEchoMode()
{
    lanrdp::HostConfigDialog::Config cfg;
    cfg.password = QStringLiteral("abcde1");
    lanrdp::HostConfigDialog dlg(cfg, nullptr);

    auto *edit = dlg.findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    auto *btn  = dlg.findChild<QToolButton *>(QStringLiteral("passwordToggleButton"));
    QVERIFY2(edit && btn, "口令框与「睁眼 / 划眼」开关都应存在");

    // 这颗按钮不带文字，说明只剩悬停提示与无障碍名字。
    QVERIFY2(!btn->toolTip().isEmpty(), "开关没有悬停提示");
    QVERIFY2(!btn->accessibleName().isEmpty(), "开关没有无障碍名字");

    // 默认必须是隐藏的：不能因为"方便"就默认明文。
    QCOMPARE(edit->echoMode(), QLineEdit::Password);
    const QImage eyeOn = btn->icon().pixmap(btn->iconSize()).toImage();
    QVERIFY2(!eyeOn.isNull(), "「显示」那一态没有图标");

    btn->click();
    QCOMPARE(edit->echoMode(), QLineEdit::Normal);
    QCOMPARE(edit->text(), QStringLiteral("abcde1"));   // 内容不能被改动

    // 两种状态必须是两张不同的图 —— 不带文字时，图形就是唯一的状态。
    const QImage eyeOff = btn->icon().pixmap(btn->iconSize()).toImage();
    QVERIFY2(!eyeOff.isNull(), "「隐藏」那一态没有图标");
    QVERIFY2(eyeOff != eyeOn, "睁开和划掉画成了同一张图，用户看不出现在是哪一态");

    btn->click();
    QCOMPARE(edit->echoMode(), QLineEdit::Password);
    QVERIFY2(btn->icon().pixmap(btn->iconSize()).toImage() == eyeOn,
             "切回隐藏之后应当回到睁开的那张图");

    // 两个对话框是一对，别只给一个装上。
    lanrdp::ConnectDialog::Config peer;
    peer.password = QStringLiteral("abcde1");
    lanrdp::ConnectDialog connect(peer, nullptr);
    auto *peerEdit = connect.findChild<QLineEdit *>(QStringLiteral("connectPasswordEdit"));
    auto *peerBtn  = connect.findChild<QToolButton *>(QStringLiteral("passwordToggleButton"));
    QVERIFY2(peerEdit && peerBtn, "【新连接…】也要有同一个开关");
    QCOMPARE(peerEdit->echoMode(), QLineEdit::Password);
    peerBtn->click();
    QCOMPARE(peerEdit->echoMode(), QLineEdit::Normal);
}

// 口令是必填项：不足 6 位时点【开始监听】不能关掉对话框，要在口令框下方说清原因。
// 「出厂默认口令」已取消 —— 它写在源码里等于没设，改成默认留空 + 当场输入。
void TestMainWindowUi::hostDialogRejectsWeakPassword()
{
    lanrdp::HostConfigDialog dlg(lanrdp::HostConfigDialog::Config{}, nullptr);
    auto *pw   = dlg.findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    auto *hint = dlg.findChild<QLabel *>(QStringLiteral("passwordErrorHint"));
    auto *box  = dlg.findChild<QDialogButtonBox *>();
    QVERIFY2(pw && hint && box, "口令框 / 报错行 / 按钮盒都应存在");

    // 默认必须是空的，而且报错行平时不占位。
    QVERIFY2(pw->text().isEmpty(), "口令默认应当为空，强制用户当场输入");
    QVERIFY2(hint->isHidden(), "报错行平时不该出现");

    int accepted = 0;
    QObject::connect(&dlg, &QDialog::accepted, &dlg, [&accepted] { ++accepted; });

    pw->setText(QStringLiteral("abc"));   // 3 位，不合规
    box->button(QDialogButtonBox::Ok)->click();
    QVERIFY2(accepted == 0, "短口令不能放行");
    QVERIFY2(!hint->isHidden(), "短口令应当显示报错");
    QVERIFY2(hint->text().contains(QStringLiteral("6 位")), qPrintable(hint->text()));

    // 改对之后：报错收起，点确定能关。
    pw->setText(QStringLiteral("abcde1"));
    QVERIFY2(hint->isHidden(), "一开始改就该把上一轮的报错收起来");
    box->button(QDialogButtonBox::Ok)->click();
    QVERIFY2(accepted == 1, "合规口令应当放行");
}

// 控制端也要求 ≥6 位：两端同版本时短口令在对端一定被拒，本地先拦能省一次无用连接。
// ⚠️ 只拦在对话框这一层 —— `--connect` 与 startConnect() 不校验（自动化入口，
// 口令要能跟着被测端走）。
void TestMainWindowUi::connectDialogRejectsWeakPassword()
{
    lanrdp::ConnectDialog dlg(lanrdp::ConnectDialog::Config{}, nullptr);
    auto *pw   = dlg.findChild<QLineEdit *>(QStringLiteral("connectPasswordEdit"));
    auto *hint = dlg.findChild<QLabel *>(QStringLiteral("connectErrorHint"));
    auto *box  = dlg.findChild<QDialogButtonBox *>();
    QVERIFY2(pw && hint && box, "口令框 / 报错行 / 按钮盒都应存在");

    QVERIFY2(pw->text().isEmpty(), "口令默认应当为空");
    QVERIFY2(hint->isHidden(), "报错行平时不该出现");

    int accepted = 0;
    QObject::connect(&dlg, &QDialog::accepted, &dlg, [&accepted] { ++accepted; });

    pw->setText(QStringLiteral("abc"));
    box->button(QDialogButtonBox::Ok)->click();
    QVERIFY2(accepted == 0, "短口令不能放行");
    QVERIFY2(!hint->isHidden(), "短口令应当显示报错");
    QVERIFY2(hint->text().contains(QStringLiteral("6 位")), qPrintable(hint->text()));

    pw->setText(QStringLiteral("abcde1"));
    QVERIFY2(hint->isHidden(), "一开始改就该把上一轮的报错收起来");
    box->button(QDialogButtonBox::Ok)->click();
    QVERIFY2(accepted == 1, "合规口令应当放行");
}

// 地址为空与口令不合规是同一类事（「这一栏没填对」），提示也该是同一个位置的同一条红字。
// 原来的做法是临时改 placeholder —— 一次性、且看起来像系统占位符而不是报错，
// 与旁边的红字形成两种风格。
void TestMainWindowUi::connectDialogRejectsEmptyAddressWithTheSameRedLabel()
{
    lanrdp::ConnectDialog dlg(lanrdp::ConnectDialog::Config{}, nullptr);
    auto *host = dlg.findChild<QLineEdit *>(QStringLiteral("peerHostEdit"));
    auto *hint = dlg.findChild<QLabel *>(QStringLiteral("connectErrorHint"));
    auto *box  = dlg.findChild<QDialogButtonBox *>();
    QVERIFY2(host && hint && box, "地址框 / 报错行 / 按钮盒都应存在");
    QVERIFY2(hint->isHidden(), "报错行平时不该出现");

    int accepted = 0;
    QObject::connect(&dlg, &QDialog::accepted, &dlg, [&accepted] { ++accepted; });

    host->setText(QString());
    box->button(QDialogButtonBox::Ok)->click();
    QVERIFY2(accepted == 0, "地址为空不能放行");
    QVERIFY2(!hint->isHidden(), "地址为空应当显示红字");
    QVERIFY2(hint->text().contains(QStringLiteral("地址")), qPrintable(hint->text()));
    QVERIFY2(host->placeholderText() != QStringLiteral("请填写对方地址"),
             "不该再用改 placeholder 的方式提示：一次性，而且会被误认成占位符");

    // 与口令栏同一套行为：一动就收起红字
    host->setText(QStringLiteral("127.0.0.1"));
    QVERIFY2(hint->isHidden(), "开始改地址就该把红字收起来");

    // 光「显示出来」还不够 —— 颜色由主题 QSS 的 #connectErrorHint 规则决定。
    // 少了那条规则，上面几条断言照样全绿，而这一行会静默退回正文色：
    // 用户看到的是一条灰色说明，而不是「填错了」。把它钉死在 danger 上。
    dlg.setStyleSheet(lanrdp::theme::applicationStyleSheet());
    hint->ensurePolished();
    QCOMPARE(hint->palette().color(QPalette::WindowText), lanrdp::theme::colors().danger);
}

// 空态大图标是按**构造时**的 DPR 栅格化的位图：窗口拖到另一块缩放比不同的屏幕上
// （100% ↔ 150%）不会自己重画，于是发糊。这里能验的是「DPR 变化事件到来时图标被
// 重画且仍与控件 DPR 一致」；真实发糊只能靠肉眼在两块屏之间拖一次。
void TestMainWindowUi::emptyStateIconFollowsDevicePixelRatio()
{
    lanrdp::EmptyState empty(nullptr);
    auto *icon = empty.findChild<QLabel *>(QStringLiteral("emptyIcon"));
    QVERIFY2(icon && !icon->pixmap().isNull(), "空态大图标应当就位");
    QVERIFY2(qFuzzyCompare(icon->pixmap().devicePixelRatio(), empty.devicePixelRatioF()),
             "图标位图的 DPR 应与控件当前 DPR 一致");

    QEvent dprEvent(QEvent::DevicePixelRatioChange);
    QCoreApplication::sendEvent(&empty, &dprEvent);
    QVERIFY2(!icon->pixmap().isNull(), "DPR 变化后必须重画，不能变成空图");
    QVERIFY2(qFuzzyCompare(icon->pixmap().devicePixelRatio(), empty.devicePixelRatioF()),
             "重画后仍应与控件 DPR 一致");
}

// startHost() 里的校验是**兜底**：对话框已经拦过一道，但命令行（--host）和
// 程序化调用都直接走到这里。口令不合规时不许有任何状态被改动。
void TestMainWindowUi::startHostRejectsWeakPassword()
{
    MainWindow w(testLogPath());

    w.startHost(0, QStringLiteral("abc"));
    QVERIFY2(!w.isHosting(), "口令不合规时不应该开始监听");
}

// 入参 -> 控件 -> config() 的往返必须一致（否则用户填的值会在某处被悄悄换掉）。
void TestMainWindowUi::dialogsRoundTripTheirConfig()
{
    // 【开始监听…】
    lanrdp::HostConfigDialog::Config hc;
    hc.port       = 9123;
    hc.password   = QStringLiteral("secret");
    hc.receiveDir = QStringLiteral("D:/tmp/recv");
    hc.useH264    = false;
    lanrdp::HostConfigDialog hostDlg(hc);

    const auto hc2 = hostDlg.config();
    QCOMPARE(hc2.port, quint16(9123));
    QCOMPARE(hc2.password, QStringLiteral("secret"));
    QCOMPARE(hc2.receiveDir, QStringLiteral("D:/tmp/recv"));
    QVERIFY2(!hc2.useH264, "H.264 复选框应当忠实反映入参");

    // 端口用 QSpinBox 而不是 QLineEdit：非法输入**根本输不进去**，
    // 省掉「运行时校验 + 报错提示」那一整套（那是白给的一类 bug）。
    auto *portBox = hostDlg.findChild<QSpinBox *>(QStringLiteral("portEdit"));
    QVERIFY(portBox);
    QVERIFY2(portBox->minimum() >= 1, "端口下限不能是 0");
    QCOMPARE(portBox->maximum(), 65535);
    portBox->setValue(0);
    QCOMPARE(portBox->value(), portBox->minimum());

    // 【新连接…】
    lanrdp::ConnectDialog::Config cc;
    cc.host     = QStringLiteral("192.168.1.9");
    cc.port     = 9500;
    cc.password = QStringLiteral("pw2");
    lanrdp::ConnectDialog connDlg(cc);

    const auto cc2 = connDlg.config();
    QCOMPARE(cc2.host, QStringLiteral("192.168.1.9"));
    QCOMPARE(cc2.port, quint16(9500));
    QCOMPARE(cc2.password, QStringLiteral("pw2"));

    // 【设置…】
    lanrdp::SettingsDialog::Config sc;
    sc.autoReconnect = false;
    sc.inputEnabled  = true;
    lanrdp::SettingsDialog setDlg(sc);

    const auto sc2 = setDlg.config();
    QVERIFY2(!sc2.autoReconnect, "自动重连开关应当忠实反映入参");
    QVERIFY2(sc2.inputEnabled, "键鼠总开关应当忠实反映入参");
}

// 【设置…】是两个「跨会话全局开关」的**唯一**编辑处，且刻意**不含**
// 端口 / 口令 / 接收目录 —— 那些属于「启动一次会话」的参数（在【开始监听…】里）。
// 同一个状态有两处可编辑，就必然有一处会忘记同步。
void TestMainWindowUi::settingsDialogHoldsTheGlobalSwitches()
{
    lanrdp::SettingsDialog dlg({});

    QVERIFY2(dlg.findChild<QAbstractButton *>(QStringLiteral("autoReconnectCheck")),
             "【设置…】应当有「断线自动重连」");
    QVERIFY2(dlg.findChild<QAbstractButton *>(QStringLiteral("inputCheck")),
             "【设置…】应当有「启用远程键鼠控制」");

    // 反证：启动参数不该出现在这里
    QVERIFY2(!dlg.findChild<QWidget *>(QStringLiteral("portEdit")),
             "端口属于【开始监听…】，不该在【设置…】里再放一份");
    QVERIFY2(!dlg.findChild<QWidget *>(QStringLiteral("recvDirEdit")),
             "接收目录属于【开始监听…】，不该在【设置…】里再放一份");
}

// 被控端**口令为空必须当场拦下**。
//
// 关键点：空口令不是「连不上」，而是「谁都能连」—— 两端口令都是空串时
// sha256("") 一致，握手**会通过**。一旦用户在启动对话框里清空口令，
// 被控端就变成一个**无凭据的远程键鼠注入服务**。
//
// 同文件的 ConnectDialog 早就覆写了 accept() 校验地址；这里只是漏做了对称的事。
void TestMainWindowUi::hostDialogRejectsEmptyPassword()
{
    lanrdp::HostConfigDialog dlg({});
    auto *pw = dlg.findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    QVERIFY2(pw, "【开始监听…】应当有口令输入框（objectName=passwordEdit）");

    pw->clear();
    dlg.accept();
    QVERIFY2(dlg.result() != int(QDialog::Accepted),
             "空口令必须被拒 —— 否则被控端会变成无凭据的远程输入服务");

    pw->setText(QStringLiteral("s3cret"));
    dlg.accept();
    QCOMPARE(dlg.result(), int(QDialog::Accepted));
}

// ---------------------------------------------------------------------------
// 基本界面结构
// ---------------------------------------------------------------------------

// 日志不再写 UI，但「清日志」这条路径必须仍然可用 —— 清的是内存缓冲。
void TestMainWindowUi::clearLogEmptiesBufferOnly()
{
    MainWindow w(testLogPath());

    w.logPort()->append(QStringLiteral("one"));
    w.logPort()->append(QStringLiteral("two"));
    QVERIFY(!w.logPort()->lines().isEmpty());

    w.clearLog();
    QVERIFY2(w.logPort()->lines().isEmpty(), "清日志后内存缓冲应为空");
}

// 没连上时不应该能点「断开」。
void TestMainWindowUi::disconnectButtonStartsDisabled()
{
    MainWindow w(testLogPath());

    auto *disconnect = barButton(w, "disconnectButton");
    QVERIFY(disconnect);
    QVERIFY(!disconnect->isEnabled());
    QCOMPARE(disconnect->accessibleName(), QStringLiteral("断开"));

    // 【断开】是危险动作：换成图标后 QSS 的 color 管不到 QIcon，颜色得落在图形上。
    // 取图标里最不透明的那个像素来比对 —— 抗锯齿边缘是半透明的，颜色会被拉淡。
    {
        const QImage img = disconnect->icon().pixmap(disconnect->iconSize()).toImage();
        QVERIFY2(!img.isNull(), "【断开】没有图标");

        QColor solid;
        int bestAlpha = 0;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const QColor px = img.pixelColor(x, y);
                if (px.alpha() > bestAlpha) {
                    bestAlpha = px.alpha();
                    solid     = px;
                }
            }
        }
        QVERIFY2(bestAlpha == 255, "【断开】的图标一个实色像素都没有");
        QCOMPARE(solid.rgb(), lanrdp::theme::colors().danger.rgb());
    }

    // 同理，【被控】菜单的监听开关初始是「开始监听…」
    auto *listenAct = w.findChild<QAction *>(QStringLiteral("actionListen"));
    QVERIFY(listenAct);
    QCOMPARE(listenAct->text(), QStringLiteral("开始监听…"));
}

// 【被控 → 开始监听…/停止监听】双态菜单项在监听起停之间切换。
void TestMainWindowUi::hostButtonTogglesListening()
{
    MainWindow w(testLogPath());

    auto *listenAct = w.findChild<QAction *>(QStringLiteral("actionListen"));
    QVERIFY(listenAct);
    QVERIFY(!w.isHosting());

    w.startHost(0, QStringLiteral("secret"));  // 0 = 系统分配临时端口
    QVERIFY(w.isHosting());
    QCOMPARE(listenAct->text(), QStringLiteral("停止监听"));

    w.stopHost();
    QVERIFY(!w.isHosting());
    QCOMPARE(listenAct->text(), QStringLiteral("开始监听…"));
}

// 【断开】的可用性跟着连接状态走：连上 -> 可用；点断开 -> 复原。
void TestMainWindowUi::disconnectButtonFollowsConnectionState()
{
    MainWindow w(testLogPath());

    // 用一个真的 HostServer 当对端（loopback），走完整的握手链路
    lanrdp::HostServer host(QStringLiteral("secret"));
    QVERIFY(host.listen(0));

    auto *disconnect = barButton(w, "disconnectButton");
    auto *connect    = barButton(w, "connectButton");
    QVERIFY(disconnect && connect);

    w.startConnect(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    QVERIFY(disconnect->isEnabled());
    // 【新连接…】不再是「连上就置灰」—— 一控多里它正是「再开一格」的入口，
    // 只在格数到上限（4）时才失效。这里顺手钉住「连上了也还没多开格」。
    QVERIFY2(connect->isEnabled(), "连上之后【新连接…】仍应可用（一控多的入口）");
    QCOMPARE(w.viewCount(), 1);
    QCOMPARE(host.sessions().size(), 1);     // 对端确实收到了一条会话

    // 点【断开】：等价于用鼠标点按钮
    QVERIFY(QMetaObject::invokeMethod(&w, "onDisconnectButtonClicked"));
    QTRY_VERIFY(!w.clientReady());
    QVERIFY(!disconnect->isEnabled());
    QVERIFY(connect->isEnabled());
    QTRY_COMPARE(host.sessions().size(), 0);
}

// 两端各一条传输进度条，常态隐藏，且落在**状态栏**上。
void TestMainWindowUi::transferProgressBarsStartHidden()
{
    MainWindow w(testLogPath());

    auto *send = w.findChild<QProgressBar *>(QStringLiteral("sendProgress"));
    auto *recv = w.findChild<QProgressBar *>(QStringLiteral("recvProgress"));
    QVERIFY2(send, "控制端应当有发送进度条 sendProgress");
    QVERIFY2(recv, "被控端应当有接收进度条 recvProgress");

    // 进度条从面板搬到状态栏 —— 放悬浮条上会让条宽随文件名 / 百分比
    // 不停跳动，而状态栏本来就在显示同类信息。
    QVERIFY2(w.statusBar()->isAncestorOf(send), "发送进度条应当挂在状态栏上");
    QVERIFY2(w.statusBar()->isAncestorOf(recv), "接收进度条应当挂在状态栏上");

    // 注意：QProgressBar 新建时 value() 是 -1（Qt 的「尚无进度」初值），
    // 所以这里不断言 0，真正的复位由 endProgress() 保证。

    // show() 之后仍 isHidden() = 被显式隐藏过（没显式隐藏的子控件此时会变可见）
    w.show();
    QVERIFY2(send->isHidden(), "发送进度条应常态隐藏，不占宽度");
    QVERIFY2(recv->isHidden(), "接收进度条应常态隐藏，不占宽度");
}

// ---------------------------------------------------------------------------
// 被控端收回输入权（双态菜单项 + 锁定语义）
// ---------------------------------------------------------------------------

// 【被控 → 收回操作权】默认可用且文案正确 —— 未监听时也能**预防性锁定**：
// 先把键鼠锁住，之后谁连上来都操作不了。
void TestMainWindowUi::takeInputButtonStartsReadyToTake()
{
    MainWindow w(testLogPath());

    auto *take = w.findChild<QAction *>(QStringLiteral("actionTakeInput"));
    QVERIFY2(take, "【被控】菜单里应当有收回操作权 actionTakeInput");
    QCOMPARE(take->text(), QStringLiteral("收回操作权"));
    QVERIFY2(take->statusTip().contains(QStringLiteral("收回")),
             "状态栏提示要把这个动作说清");
    QVERIFY(take->isEnabled());
    QVERIFY2(!w.inputLocked(), "默认不该处于锁定态");
}

// 同一个菜单项点一次 = 锁定（文案换成「恢复操作」），再点一次 = 解锁。
void TestMainWindowUi::takeInputButtonTogglesLockState()
{
    MainWindow w(testLogPath());

    auto *take = w.findChild<QAction *>(QStringLiteral("actionTakeInput"));
    QVERIFY(take);

    // ⚠️ 宿主跑在**工作线程**上 ⇒ 状态要等一次往返才更新
    //（触发 → 投递到宿主 → 宿主推快照回来）。
    take->trigger();
    QTRY_VERIFY2(w.inputLocked(), "点一次应当锁定");
    QCOMPARE(take->text(), QStringLiteral("恢复操作"));

    take->trigger();
    QTRY_VERIFY2(!w.inputLocked(), "再点一次应当解锁");
    QCOMPARE(take->text(), QStringLiteral("收回操作权"));
}

// 锁定是粘性的：开始监听不会清锁；但【停止监听】会清锁，
// 否则会留下「一个控制端都看不见、输入却被锁着」的死状态。
void TestMainWindowUi::stopHostClearsInputLock()
{
    MainWindow w(testLogPath());

    auto *take = w.findChild<QAction *>(QStringLiteral("actionTakeInput"));
    QVERIFY(take);

    take->trigger();
    QTRY_VERIFY(w.inputLocked());

    w.startHost(0, QStringLiteral("secret"));  // 0 = 系统分配临时端口
    QTRY_VERIFY(w.inputLocked());

    w.stopHost();
    QTRY_VERIFY(!w.inputLocked());
}

// 改端口重新开始监听：必须真的监听在新端口上，旧端口要放掉。
//
// 说明：当前 `MainWindow::startHost` 每次都先 `stopHost()` 再 `new HostServer`，
// 所以这条在修复前后都会通过 —— 它是**锁调用方契约**（防将来把 server 改成复用、
// 又把新端口静默丢掉），不是修复本身的红用例；红用例在 `tests/test_hostserver.cpp`。
void TestMainWindowUi::startHostOnNewPortRebinds()
{
    MainWindow w(testLogPath());

    QVector<quint16> ports;
    QObject::connect(&w, &MainWindow::listening, &w, [&ports](quint16 p) { ports.append(p); });

    w.startHost(0, QStringLiteral("secret"));   // 0 = 系统分配临时端口
    QTRY_VERIFY(!ports.isEmpty());
    const quint16 first = ports.last();
    QVERIFY(first != 0);

    // 挑一个与当前不同的具体端口
    quint16 second = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::Any, 0));
        second = probe.serverPort();
        probe.close();
    }
    QVERIFY(second != first);

    w.startHost(second, QStringLiteral("secret"));
    QTRY_COMPARE(ports.last(), second);

    // 新端口真的可连 —— 而不是只改了个账面数字
    QTcpSocket sock;
    sock.connectToHost(QHostAddress::LocalHost, second);
    QVERIFY2(sock.waitForConnected(3000), qPrintable(sock.errorString()));

    // 旧端口已释放：能再单独绑回去（若还留着旧监听，这里会 AddressInUse）
    QTcpServer reuser;
    QVERIFY2(reuser.listen(QHostAddress::Any, first), qPrintable(reuser.errorString()));
}

// 端到端：真的开两个 MainWindow（一个被控端、一个控制端）走 loopback，
// 验证「机主收回输入权 → 控制端立刻失去令牌 → 锁定期间拿不回来 → 解锁后恢复」。
void TestMainWindowUi::endToEndTakeInputLocksOutClient()
{
    MainWindow host(testLogPath());
    quint16    hostPort = 0;
    QObject::connect(&host, &MainWindow::listening, &host,
                     [&hostPort](quint16 p) { hostPort = p; });
    host.startHost(0, QStringLiteral("secret"));
    QTRY_VERIFY(hostPort != 0);

    // 【被控 → 收回操作权】菜单项（双态文案）
    auto *take = host.findChild<QAction *>(QStringLiteral("actionTakeInput"));
    QVERIFY(take);
    QCOMPARE(take->text(), QStringLiteral("收回操作权"));

    MainWindow client(testLogPath());
    client.startConnect(QStringLiteral("127.0.0.1"), hostPort, QStringLiteral("secret"));
    QTRY_VERIFY(client.clientReady());
    QTRY_VERIFY2(client.hasToken(), "连接成功后控制端应当自动拿到输入令牌");

    // ① 收回：控制端应当立刻失去令牌
    take->trigger();
    QTRY_VERIFY2(host.inputLocked(), "机主收回后应当处于锁定态");
    QTRY_VERIFY2(!client.hasToken(), "机主收回输入权后控制端应当失去令牌");

    // ② 这里的语义是「收回之后不再自动重取令牌」——发起的动作必须是
    // 唯一会自动申请令牌的那条路径（鼠标按下），而不是鼠标移动：
    // 鼠标移动只是「想知道镜头那边能不能动」，没有输入权时它根本不发帧、也不申请令牌。
    client.sendMouseMove(32768, 32768);
    QTest::qWait(300);
    QVERIFY2(!client.hasToken(), "锁定期间鼠标移动不该申请令牌（它压根不发输入帧）");

    client.sendMouseButton(0, true);
    client.sendMouseButton(0, false);
    QTest::qWait(300);
    QVERIFY2(!client.hasToken(), "锁定期间不该重新授予令牌");

    // ③ 解锁后恢复。
    // 注意这里必须先等到「对端明确表态」（收到 TokenRelease）才可能重新申请 ——
    // 这是防刷屏那条改动的刻意代价：一次申请失败后不再自动重试，否则控制端
    // 按住按键就能把机主的日志刷爆。上面 ② 收到的 TokenRelease 已经把闩解开了，
    // 所以这里再点一次即触发下一轮申请。
    take->trigger();
    QTRY_VERIFY2(!host.inputLocked(), "再点一次应当解锁");
    QTRY_VERIFY2(client.canRequestToken(), "收到对方的明确表态后应当允许再申请一次");
    client.sendMouseButton(0, true);
    client.sendMouseButton(0, false);
    QTRY_VERIFY2(client.hasToken(), "解锁后控制端应当能重新拿到令牌");
}

// 控制端条上那颗操作权图标：**没有文字**，所以「我现在能不能操作对方」
// 全靠图形 + 悬停提示表达。这里走真实连接核对三态：
//   未连接 → 已获得 → 等待中（机主收回）→ 已获得（机主放开）。
// 顺带守住【让出操作权】：只有握着令牌时才可点。
void TestMainWindowUi::tokenStatusIconFollowsToken()
{
    MainWindow host(testLogPath());
    quint16    hostPort = 0;
    QObject::connect(&host, &MainWindow::listening, &host,
                     [&hostPort](quint16 p) { hostPort = p; });
    host.startHost(0, QStringLiteral("secret"));
    QTRY_VERIFY(hostPort != 0);

    MainWindow client(testLogPath());

    auto *badge = qobject_cast<QToolButton *>(barButton(client, "tokenStatusButton"));
    auto *release = barButton(client, "releaseTokenButton");
    QVERIFY2(badge, "控制端条上应当有操作权状态图标");
    QVERIFY2(release, "控制端条上应当有让出操作权按钮");
    QVERIFY2(badge->text().isEmpty(), "状态图标不该带文字");
    QCOMPARE(badge->accessibleName(), QStringLiteral("操作权：未连接"));

    client.startConnect(QStringLiteral("127.0.0.1"), hostPort, QStringLiteral("secret"));
    QTRY_VERIFY(client.hasToken());

    QTRY_COMPARE(badge->accessibleName(), QStringLiteral("操作权：已获得"));
    QTRY_VERIFY2(release->isEnabled(), "握着操作权时【让出操作权】应当可点");
    const QImage held = badge->icon().pixmap(QSize(18, 18)).toImage();
    QVERIFY2(!held.isNull(), "状态图标没画出来 —— 那这颗按钮就是个空白");

    // 机主收回 ⇒ 控制端还在线、但已经没有操作权了：状态要落到「等待中」。
    auto *take = host.findChild<QAction *>(QStringLiteral("actionTakeInput"));
    QVERIFY(take);
    take->trigger();
    QTRY_VERIFY(!client.hasToken());
    QTRY_COMPARE(badge->accessibleName(), QStringLiteral("操作权：等待中"));
    QTRY_VERIFY2(!release->isEnabled(), "没有操作权时【让出操作权】点不了");

    const QImage waiting = badge->icon().pixmap(QSize(18, 18)).toImage();
    QVERIFY2(waiting != held, "「已获得」与「等待中」画成了同一张图，用户看不出区别");
}

// 文件传输与剪贴板同级受输入令牌门控 —— 只有「当前在操作这台机器」的控制端
// 才有资格让被控端落盘。
//
// 本条补的是 P26 在 MainWindow 层的缺口：`test_filetransfer_e2e` 直接把帧喂给
// 接收器（测的是来源隔离），测不到这道门；真机 CLI 也构造不出「先到先得」以外的
// 情形 —— 令牌是**每帧**判的，没令牌那一路的帧会被整条丢掉。
//
// 场景：甲先连（自动拿到令牌）传一个大文件；乙后连（没有令牌）也传一个
//      ⇒ 甲的文件完整落盘，乙的一个字节都没落地；
//      甲交还令牌、乙取得令牌后再传 ⇒ 这次落盘成功（「取得输入权后即可传」）。
void TestMainWindowUi::fileTransferFollowsInputToken()
{
    QTemporaryDir sendDir;
    QTemporaryDir recvDir;
    QVERIFY(sendDir.isValid() && recvDir.isValid());

    MainWindow host(testLogPath());
    quint16    hostPort = 0;
    QObject::connect(&host, &MainWindow::listening, &host,
                     [&hostPort](quint16 p) { hostPort = p; });
    host.setReceiveDir(recvDir.path());
    host.startHost(0, QStringLiteral("secret"));
    QTRY_VERIFY(hostPort != 0);

    QVector<bool>    receivedOk;
    QVector<QString> receivedPath;
    QObject::connect(&host, &MainWindow::fileReceived, &host,
                     [&receivedOk, &receivedPath](bool ok, const QString &path, const QString &) {
                         receivedOk.append(ok);
                         receivedPath.append(path);
                     });

    MainWindow a(testLogPath());   // 甲：先连先得
    a.startConnect(QStringLiteral("127.0.0.1"), hostPort, QStringLiteral("secret"));
    QTRY_VERIFY(a.clientReady());
    QTRY_VERIFY2(a.hasToken(), "先连上的控制端应当自动拿到输入令牌");

    MainWindow b(testLogPath());   // 乙：令牌已被甲占着
    b.startConnect(QStringLiteral("127.0.0.1"), hostPort, QStringLiteral("secret"));
    QTRY_VERIFY(b.clientReady());
    QVERIFY2(!b.hasToken(), "令牌已被甲持有，乙不该拿到");

    const QByteArray contentA = makeBytes(4 * 1024 * 1024 + 777);
    const QByteArray contentB = makeBytes(3 * 1024 * 1024 + 123);
    const QString    pathA    = writeFileInto(sendDir.path(), QStringLiteral("a.bin"), contentA);
    const QString    pathB    = writeFileInto(sendDir.path(), QStringLiteral("b.bin"), contentB);
    QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

    // 两路几乎同时开传
    QSignalSpy bSent(&b, &MainWindow::fileSent);
    QVERIFY(a.sendFile(pathA));
    QVERIFY(b.sendFile(pathB));

    // 甲：完整落盘
    QTRY_VERIFY_WITH_TIMEOUT(!receivedOk.isEmpty(), 30000);
    QVERIFY2(receivedOk.at(0), qPrintable(receivedPath.at(0)));
    QCOMPARE(QFileInfo(receivedPath.at(0)).fileName(), QStringLiteral("a.bin"));

    // 乙：控制端照样会报「发送完成」（帧确实发出去了），但被控端一个字节都不收
    QTRY_COMPARE_WITH_TIMEOUT(bSent.count(), 1, 30000);
    QTest::qWait(300);
    QCOMPARE(receivedOk.size(), 1);   // 只有甲那一次
    {
        const QDir        dir(recvDir.path());
        const QStringList files = dir.entryList(QDir::Files);
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.first(), QStringLiteral("a.bin"));
        QCOMPARE(lanrdp::sha256Of(readWholeFile(dir.absoluteFilePath(files.first()))),
                 lanrdp::sha256Of(contentA));
    }

    // 交接：甲交还令牌 → 乙点一下（鼠标按下是唯一会自动申请令牌的路径）
    a.releaseToken();
    QTRY_VERIFY2(!a.hasToken(), "甲交还后应当失去令牌");
    // ⚠️ 甲交还的那一帧还在路上：被控端处理完之前，乙的申请会被拒（还会回一帧
    //    TokenRelease 把乙的申请闩解开）。所以「点一下」要允许点几次 ——
    //    这与界面上的实际行为一致（抢不到再点一下就行）。
    for (int i = 0; i < 30 && !b.hasToken(); ++i) {
        b.sendMouseButton(0, true);
        b.sendMouseButton(0, false);
        QTest::qWait(100);
    }
    QVERIFY2(b.hasToken(), "甲交还后乙应当能拿到令牌");

    // 取得输入权后即可传
    QVERIFY(b.sendFile(pathB));
    QTRY_COMPARE_WITH_TIMEOUT(receivedOk.size(), 2, 30000);
    QVERIFY2(receivedOk.at(1), qPrintable(receivedPath.at(1)));
    QCOMPARE(QFileInfo(receivedPath.at(1)).fileName(), QStringLiteral("b.bin"));
    QCOMPARE(lanrdp::sha256Of(readWholeFile(receivedPath.at(1))), lanrdp::sha256Of(contentB));
}

// 令牌身份与裸 `Session*` 解耦。
//
// 从前 TokenGate 直接拿 `Session*` 当身份：会话销毁后新会话若落在同一地址，
// 就会「继承」旧持有者的令牌（`holds(新会话)` 会返回 true）。现在身份是
// TokenGate 发放的单调 id + 一张 `Session* -> id` 表，会话销毁时必须清表。
//
// 这条钉住「上一任断开 -> 令牌空出 -> 新会话能拿到」整条路径，
// 覆盖 clientDisconnected / stopHost 里的身份表清理（漏了清表就可能让
// 复用地址的新会话查到陈旧映射）。
void TestMainWindowUi::disconnectedHolderDoesNotBlockNextSession()
{
    MainWindow host(testLogPath());
    quint16    hostPort = 0;
    QObject::connect(&host, &MainWindow::listening, &host,
                     [&hostPort](quint16 p) { hostPort = p; });
    host.startHost(0, QStringLiteral("secret"));
    QTRY_VERIFY(hostPort != 0);

    {
        MainWindow first(testLogPath());
        first.startConnect(QStringLiteral("127.0.0.1"), hostPort, QStringLiteral("secret"));
        QTRY_VERIFY(first.clientReady());
        QTRY_VERIFY2(first.hasToken(), "先连上的控制端应当自动拿到令牌");

    // 被控端确实登记了这个会话（「已连 N 个」由 refreshClientList 逐格刷，
    // 悬浮条上那颗按钮 objectName 沿用 clientListButton）
    QTRY_VERIFY(host.findChild<QAbstractButton *>(QStringLiteral("clientListButton"))->text()
                == QStringLiteral("1"));
    }   // first 析构 -> 断连

    // 等被控端把断连处理完（释放令牌 + 清身份记录）：客户端列表回到 0
    QTRY_VERIFY_WITH_TIMEOUT(
        host.findChild<QAbstractButton *>(QStringLiteral("clientListButton"))->text()
            == QStringLiteral("0"), 10000);

    MainWindow second(testLogPath());
    second.startConnect(QStringLiteral("127.0.0.1"), hostPort, QStringLiteral("secret"));
    QTRY_VERIFY(second.clientReady());

    // 令牌应当已空出；「点一下」是控制端自动申请令牌的路径（可能需要几次，
    // 与界面上「抢不到再点一下」的实际行为一致）。
    for (int i = 0; i < 30 && !second.hasToken(); ++i) {
        second.sendMouseButton(0, true);
        second.sendMouseButton(0, false);
        QTest::qWait(100);
    }
    QVERIFY2(second.hasToken(), "上一任断开后令牌应当空出，新会话必须能拿到");
}

// `--send-file` 的收尾契约 —— 真机 CLI 上「控制端报发送完成、被控端什么都没有」的根因。
//
// 两件实测过、别想当然的事：
// ① `fileSent` 只表示「帧已排进 Qt 的用户态写缓冲」—— 实测一个 256 KB 的文件
//    在这一刻缓冲里还有 262309 字节没交给内核，几 MB 的文件更甚。所以
//    「等一个固定毫秒数再退出」必然不可靠（分界点随链路快慢漂移）。
// ② 缓冲排空后直接 `disconnectFromHost()` 也不行：Windows 关闭一个**读缓冲非空**
//    的 socket 会回 RST（对端一直在推画面，读缓冲几乎总是非空），RST 会连带丢掉
//    本方还没被确认的发送队列 —— 对端只收到文件头就报「传输中断」。
//
// 正确收尾 = `halfCloseClientWhenFlushed()`（等排空 + 半关写方向）。这条钉死它。
void TestMainWindowUi::sendFileCliShutdownDeliversLargeFile()
{
    QTemporaryDir sendDir;
    QTemporaryDir recvDir;
    QVERIFY(sendDir.isValid() && recvDir.isValid());

    MainWindow host(testLogPath());
    quint16    hostPort = 0;
    QObject::connect(&host, &MainWindow::listening, &host,
                     [&hostPort](quint16 p) { hostPort = p; });
    host.setReceiveDir(recvDir.path());
    host.startHost(0, QStringLiteral("secret"));
    QTRY_VERIFY(hostPort != 0);

    QVector<bool>    receivedOk;
    QVector<QString> receivedPath;
    QObject::connect(&host, &MainWindow::fileReceived, &host,
                     [&receivedOk, &receivedPath](bool ok, const QString &path, const QString &) {
                         receivedOk.append(ok);
                         receivedPath.append(path);
                     });

    MainWindow client(testLogPath());
    client.startConnect(QStringLiteral("127.0.0.1"), hostPort, QStringLiteral("secret"));
    QTRY_VERIFY(client.clientReady());

    const QByteArray content = makeBytes(4 * 1024 * 1024 + 999);
    const QString    path    = writeFileInto(sendDir.path(), QStringLiteral("cli.bin"), content);
    QVERIFY(!path.isEmpty());

    QSignalSpy sent(&client, &MainWindow::fileSent);
    QVERIFY(client.sendFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(sent.count(), 1, 30000);
    QVERIFY2(sent.at(0).at(0).toBool(), "发送侧应当报成功");

    QSignalSpy closed(&client, &MainWindow::clientClosed);
    client.halfCloseClientWhenFlushed();   // CLI 的收尾

    // 对端必须收全 —— 这正是「固定等待」和「直接 close」做不到的
    QTRY_COMPARE_WITH_TIMEOUT(receivedOk.size(), 1, 60000);
    QVERIFY2(receivedOk.at(0), qPrintable(receivedPath.at(0)));
    QCOMPARE(QFileInfo(receivedPath.at(0)).fileName(), QStringLiteral("cli.bin"));
    QCOMPARE(lanrdp::sha256Of(readWholeFile(receivedPath.at(0))), lanrdp::sha256Of(content));

    // 收尾要真的走完（对端读到 EOF 会关掉它那一侧）
    QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 30000);
}

// ---------------------------------------------------------------------------
// 断线自动重连
// ---------------------------------------------------------------------------

// 「断线自动重连」默认开（它只存在于【设置…】里，主窗口只留状态查询）。
void TestMainWindowUi::autoReconnectDefaultsOn()
{
    MainWindow w(testLogPath());
    QVERIFY2(w.autoReconnectEnabled(), "按设计默认开启");

    // 对话框打开时读的就是这个状态当初始值
    lanrdp::SettingsDialog dlg({ w.autoReconnectEnabled(), false });
    auto *check = dlg.findChild<QAbstractButton *>(QStringLiteral("autoReconnectCheck"));
    QVERIFY(check);
    QVERIFY2(check->isChecked(), "对话框初值应当反映当前状态");
}

// 连不上的时候也要排重连（否则「先开控制端、后开被控端」这种顺序就没法自动恢复）。
// 同时验证：重连待命期间【断开】必须可用，否则用户没有中止的入口。
void TestMainWindowUi::failedConnectSchedulesReconnect()
{
    MainWindow w(testLogPath());

    auto *disconnect = barButton(w, "disconnectButton");
    QVERIFY(disconnect);
    QVERIFY(!disconnect->isEnabled());
    QVERIFY(!w.reconnectPending());

    const quint16 deadPort = pickDeadPort();
    QVERIFY(deadPort != 0);

    w.startConnect(QStringLiteral("127.0.0.1"), deadPort, QStringLiteral("secret"));

    // 会话一建好就该可点：此时还没握手（clientReady_ 仍是 false），
    // 靠的是 updateConnectionUi() 把 clientSession_ 也算进去。
    // 不刷的话状态会停在 teardownClient() 那一刻 —— 那段窗口期就没有出口了。
    QVERIFY2(disconnect->isEnabled(),
             "会话建立后【断开】就应当可点（等待握手期间不能没有中止出口）");

    QTRY_VERIFY2(w.reconnectPending(), "连接失败后应当安排自动重连");
    QVERIFY2(!w.clientReady(), "这次连接并没有成功");
    QVERIFY2(disconnect->isEnabled(), "重连待命时【断开】应当可用（用来中止重连）");
}

// 用户点了【断开】就得真的停 —— 不能过一会又自己连回来。
void TestMainWindowUi::userDisconnectCancelsPendingReconnect()
{
    MainWindow w(testLogPath());

    const quint16 deadPort = pickDeadPort();
    QVERIFY(deadPort != 0);

    w.startConnect(QStringLiteral("127.0.0.1"), deadPort, QStringLiteral("secret"));
    QTRY_VERIFY(w.reconnectPending());

    QVERIFY(QMetaObject::invokeMethod(&w, "onDisconnectButtonClicked"));
    QVERIFY2(!w.reconnectPending(), "用户主动断开后不该再继续重连");

    // 第一次退避是 1 s；等 1.5 s 确认没有新的重连被悄悄排上
    QTest::qWait(1500);
    QVERIFY2(!w.reconnectPending(), "主动断开后不该有新的重连被安排");
}

// 手工验证时发现的瑕疵：重连待命期间点【断开】，「不再重连」本来就是对的，
// 但界面没跟着复位（按钮还亮着、日志也没有），用户看不出到底中止了没有。
// 这里把「真的连上 -> 对端断开 -> 排上重连」这条链走完整，
// 因为它才会产生「重连待命 + 已经没有活着的会话」这个前置状态
// （连不上的情况 clientSession_ 仍在，走的是另一条分支）。
void TestMainWindowUi::userDisconnectWhileReconnectPendingResetsUi()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer host(QStringLiteral("secret"));
    QVERIFY(host.listen(0));

    auto *disconnect = barButton(w, "disconnectButton");
    auto *connect    = barButton(w, "connectButton");
    QVERIFY(disconnect && connect);

    w.startConnect(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());

    // 对端断开会话（顺便关掉监听，免得万一退避到点真的又连上了，
    // 那 reconnectPending() 会瞬间变 false，测试就飘了）
    host.dropAllClients();
    host.stop();

    QTRY_VERIFY2(w.reconnectPending(), "对端断开后应当安排重连");
    QVERIFY2(!w.clientReady(), "连接已经没了");
    QVERIFY2(disconnect->isEnabled(), "重连待命时【断开】必须可点（用户的中止入口）");

    // 判据从 UI 控件改为**日志端口** —— 日志不再写界面，
    // 但「中止重连要留下反馈」这条要求不变。
    w.clearLog();
    QVERIFY(QMetaObject::invokeMethod(&w, "onDisconnectButtonClicked"));

    QVERIFY2(!w.reconnectPending(), "点了【断开】就不该再重连");
    QVERIFY2(!disconnect->isEnabled(), "中止重连后【断开】应当变灰 —— 界面要跟着复位");
    QVERIFY2(connect->isEnabled(), "中止之后应当可以重新手动连接");

    bool hasFeedback = false;
    for (const QString &line : w.logPort()->lines()) {
        if (line.contains(QStringLiteral("已断开与被控端的连接"))) {
            hasFeedback = true;
            break;
        }
    }
    QVERIFY2(hasFeedback, "中止重连也要留下日志，否则用户点了看不出反馈");
}

// 关掉开关后连不上就安静地失败，不再重连。
void TestMainWindowUi::uncheckedAutoReconnectDoesNotSchedule()
{
    MainWindow w(testLogPath());

    const quint16 deadPort = pickDeadPort();
    QVERIFY(deadPort != 0);

    // ① 正向前提：先证明**同一条失败路径**在开关打开时确实会排上重连。
    // 少了这一步，下面那条「没安排重连」在「startConnect 压根没跑起来」时也会绿 ——
    // 断言的是「没发生什么」，而没有任何东西证明它**本来会发生**。
    QVERIFY2(w.autoReconnectEnabled(), "默认应当是开着的（本用例的对照基线）");
    w.startConnect(QStringLiteral("127.0.0.1"), deadPort, QStringLiteral("secret"));
    QTRY_VERIFY2(w.reconnectPending(), "开关开着时连不上就该排重连（否则本用例测不到东西）");
    QVERIFY(QMetaObject::invokeMethod(&w, "onDisconnectButtonClicked"));   // 清场
    QVERIFY(!w.reconnectPending());

    // ② 关掉开关后连不上就安静地失败，不再重连。
    // 开关控件搬进【设置…】了，程序化路径走这个 setter
    //（与 setHostCodec / setReceiveDir 同类，供自测与单测用）。
    w.setAutoReconnectEnabled(false);
    QVERIFY(!w.autoReconnectEnabled());

    w.startConnect(QStringLiteral("127.0.0.1"), deadPort, QStringLiteral("secret"));

    // 等足够久让 errorOccurred 走完，确认没有重连被安排
    QTest::qWait(500);
    QVERIFY2(!w.reconnectPending(), "关掉开关后不该安排重连");
}

// 跨机验收（2026-09-21）现场日志：机主点【收回输入权】把输入锁上之后，
// 控制端在画面上随便动一动，被控端日志就刷出**两万多行**「输入令牌请求被拒（输入已锁定）」。
// 根因是控制端每条输入事件都会发一帧 TokenReq，而对端每条都记一行日志。
// 修法两条：① 申请侧「在途去重」② 被控端拒绝时回一帧 TokenRelease 当明确答复。
// 这个用例盯的是「按住按键猛发输入，被控端最多只多出极少量拒绝日志」。
void TestMainWindowUi::lockedInputDoesNotFloodTokenRequests()
{
    MainWindow host(testLogPath());
    quint16    hostPort = 0;
    QObject::connect(&host, &MainWindow::listening, &host,
                     [&hostPort](quint16 p) { hostPort = p; });
    host.startHost(0, QStringLiteral("secret"));
    QTRY_VERIFY(hostPort != 0);

    auto *take = host.findChild<QAction *>(QStringLiteral("actionTakeInput"));
    QVERIFY(take);

    MainWindow client(testLogPath());
    client.startConnect(QStringLiteral("127.0.0.1"), hostPort, QStringLiteral("secret"));
    QTRY_VERIFY(client.clientReady());
    QTRY_VERIFY2(client.hasToken(), "连接成功后控制端应当自动拿到输入令牌");

    // 机主收回输入权并锁定 —— 这就是现场日志里那个状态
    take->trigger();
    QTRY_VERIFY2(host.inputLocked(), "收回后应当处于锁定态");
    QTRY_VERIFY2(!client.hasToken(), "收回后控制端应当失去令牌");

    auto *hostLog = host.logPort();   // 判据改读日志端口（UI 已无日志区）
    host.clearLog();

    // 模拟「用户按住按键 / 猛点鼠标」：连发 200 条输入
    for (int i = 0; i < 200; ++i) {
        client.sendMouseButton(0, i % 2 == 0);
    }
    QTest::qWait(500);

    int denyCount = 0;
    for (const QString &line : hostLog->lines()) {
        if (line.contains(QStringLiteral("操作权请求被拒")))
            ++denyCount;
    }
    // 正向前提：本用例证的是「拒绝不至于刷屏」，前提是**确实发生过拒绝** ——
    // 若链路没跑通（一次都没拒），下面那条 `<= 5` 会毫无意义地通过。
    QVERIFY2(denyCount > 0, "按住按键应当至少触发一次令牌拒绝，否则这条断言是空过的");
    QVERIFY2(denyCount <= 5,
             qPrintable(QStringLiteral("按住按键不该刷爆被控端日志，实际被拒 %1 次")
                            .arg(denyCount)));
    QVERIFY2(!client.hasToken(), "锁定期间拿不到令牌");
}

// ---------------------------------------------------------------------------
// 模式锁定 —— 一个进程同一时刻只能是「被控端」或「控制端」。
// ---------------------------------------------------------------------------

// 启动时未监听：【被控】菜单两项都在、文案正确。
void TestMainWindowUi::modeMenuStartsUnselected()
{
    MainWindow w(testLogPath());

    auto *listenAct = w.findChild<QAction *>(QStringLiteral("actionListen"));
    auto *takeAct   = w.findChild<QAction *>(QStringLiteral("actionTakeInput"));
    QVERIFY2(listenAct && takeAct, "菜单栏【被控】下应当有两个入口");

    QVERIFY2(!w.isHosting(), "启动时不应在监听");
    QCOMPARE(listenAct->text(), QStringLiteral("开始监听…"));
    QCOMPARE(takeAct->text(), QStringLiteral("收回操作权"));
    QVERIFY2(takeAct->isEnabled(), "收回操作权未监听时也应可用（预防性锁定）");
}

// 启动空态 + 连接后的条上控件：引导在空态层里，连接后条上换控制端操作组。
void TestMainWindowUi::modeActionSwitchesOverlayBar()
{
    MainWindow w(testLogPath());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto *sendFile = barButton(w, "sendFileButton");
    auto *renderer = w.screenRenderer();
    QVERIFY(sendFile && renderer);

    QVERIFY2(!w.isHosting(), "启动时不应在监听");
    // 引导已经从「画面里的一行字」搬到空态层（两个真入口）。断言的**意图**不变：
    // 这时必须有东西告诉用户接下来能做什么 —— 只是不再靠画面控件那行字。
    auto *empty = w.viewAt(0)->emptyState();
    QVERIFY2(empty && empty->isVisible(), "未连接的格应当显示空态层");
    auto *emptyTitle = empty->findChild<QLabel *>(QStringLiteral("emptyTitle"));
    QVERIFY2(emptyTitle && !emptyTitle->text().isEmpty(), "空态层应当有标题文案");
    QVERIFY2(renderer->idleText().isEmpty(),
             "空格的文字只该在空态层里 —— 画面控件再画一遍会在按钮后面透出来");

    // 连出去（连不上也行）：这一格不再空 —— 空态退场、控制端操作组上条。
    w.startConnect(QStringLiteral("127.0.0.1"), pickDeadPort(), QStringLiteral("secret"));
    QVERIFY2(!empty->isVisible(), "有连接对象的格不该显示空态层");
    QVERIFY2(shown(sendFile), "连接后应当显示【发送文件…】");
    QVERIFY2(renderer->idleText().contains(QStringLiteral("等待远程画面")),
             "连接格的空态文案应当是「等待远程画面」");
}

// 核心新行为：**被控与控制可以并存** —— 监听中的同时照样连出去，
// 各格的连接与全局监听互不干扰。
void TestMainWindowUi::hostingLocksModeEntry()
{
    // 对端：一个真的 HostServer（loopback），让连接能真正建立
    lanrdp::HostServer peer(QStringLiteral("secret"));
    QVERIFY(peer.listen(0));

    MainWindow w(testLogPath());

    // 先当被控端
    w.startHost(0, QStringLiteral("secret"));   // 0 = 让系统分配临时端口
    QVERIFY(w.isHosting());
    QVERIFY(w.hostSessionActive());

    // 监听中照样连出去 —— 不再被「模式」拦下
    w.startConnect(QStringLiteral("127.0.0.1"), peer.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    QVERIFY2(w.isHosting(), "连接出去不应影响监听状态");
    QVERIFY2(w.clientSessionActive(), "控制会话应当同时在跑");

    // 停止监听也不动已经连着的格子
    w.stopHost();
    QVERIFY2(!w.isHosting(), "停止监听后不应再监听");
    QVERIFY2(w.clientReady(), "已建立的连接不该被停止监听拆掉");
}

// 反向并存：先连出去，再开始监听 —— 被控不抢窗口、不拆格子。
void TestMainWindowUi::clientConnectLocksModeEntry()
{
    MainWindow w(testLogPath());

    auto *disc = barButton(w, "disconnectButton");
    QVERIFY(disc);

    // 连一个刚空出来的端口：连不上，但会排上重连
    w.startConnect(QStringLiteral("127.0.0.1"), pickDeadPort(), QStringLiteral("secret"));
    QVERIFY2(w.clientSessionActive(), "连不上也应算「控制会话活动中」（重连待命）");

    // 重连待命期间照样可以开始监听
    w.startHost(0, QStringLiteral("secret"));
    QVERIFY2(w.isHosting(), "控制会话进行中也应允许开始监听");

    // 断开控制端 = 只影响那一格，监听照旧
    QVERIFY(disc->isEnabled());
    disc->click();
    QVERIFY2(!w.clientSessionActive(), "断开后不该再有活动会话");
    QVERIFY2(w.isHosting(), "断开控制端不应影响监听");
}

// 监听起停**不改变任何一格**的控件组 —— 被控是全局功能，格子只看自己的连接。
void TestMainWindowUi::lockedModeHidesOtherModeActions()
{
    MainWindow w(testLogPath());
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto *sendFile = barButton(w, "sendFileButton");
    QVERIFY(sendFile);

    // 空格：没有连接，控制端操作组不该出现
    QVERIFY2(!shown(sendFile), "空格不该显示【发送文件…】");

    w.startHost(0, QStringLiteral("secret"));
    QVERIFY(w.isHosting());
    QVERIFY2(!shown(sendFile), "开始监听不改变格子状态 —— 空格还是空格");

    w.stopHost();
    QVERIFY2(!shown(sendFile), "停止监听同样不改变格子状态");
}

// ---------------------------------------------------------------------------
// 一控多（每格一条连接 + 选中路由）
// ---------------------------------------------------------------------------

// 第二台 = 第二格：连上之后再点【新连接…】应该**加一格**，而不是把第一台挤掉。
// （「连第二台会先拆掉第一台」正是重构之前的老毛病。）
void TestMainWindowUi::secondConnectOpensSecondCell()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    QCOMPARE(w.viewCount(), 1);           // 启动就有第 0 格（本机预览格）
    QCOMPARE(w.maxViews(), 4);

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    QCOMPARE(w.viewCount(), 1);           // 第一台复用第 0 格
    QCOMPARE(hostA.sessions().size(), 1);

    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);      // 第二台：新开一格
    QTRY_VERIFY(w.clientReady());

    // 两台都还活着 —— 谁也不该被谁挤掉
    QCOMPARE(hostA.sessions().size(), 1);
    QCOMPARE(hostB.sessions().size(), 1);
    QCOMPARE(w.viewGrid()->viewCount(), 2);
    QCOMPARE(w.selectedView(), w.viewAt(1));   // 新格自动成为焦点
}

// 选中跟着网格走：「跟随选中」的那几个查询与**按格**的动作都指对地方。
void TestMainWindowUi::connectCellSelectionFollowsGrid()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    // 新格是焦点：跟随选中的三个查询都应当落到第 1 格
    QCOMPARE(w.selectedView(), w.viewAt(1));
    QCOMPARE(w.screenRenderer(), w.viewAt(1)->renderer());
    QCOMPARE(w.overlayBar(), w.viewAt(1)->bar());
    QVERIFY2(w.viewAt(1)->isSelected(), "选中的那一格应当在条上画出强调线");
    QVERIFY2(!w.viewAt(0)->isSelected(), "没选中的格子不该带强调线");

    // 切到第 0 格：整套查询跟着走
    w.viewGrid()->setSelectedIndex(0);
    QCOMPARE(w.selectedView(), w.viewAt(0));
    QCOMPARE(w.screenRenderer(), w.viewAt(0)->renderer());
    QCOMPARE(w.overlayBar(), w.viewAt(0)->bar());
    QVERIFY(w.viewAt(0)->isSelected());
    QVERIFY(!w.viewAt(1)->isSelected());

    // 每格管好自己的连接：断开第 0 格不该动第 1 格
    w.viewAt(0)->disconnectButton()->click();
    QTRY_COMPARE(hostA.sessions().size(), 0);
    QCOMPARE(hostB.sessions().size(), 1);
    QVERIFY2(w.viewAt(1)->connection() && w.viewAt(1)->connection()->ready(),
             "断开第 0 格不该影响第 1 格");
}

// 「点哪一格就操作哪一格」的真正入口：点击**必须**变成选中，同时**不能被吞掉**
// —— 画面区的按键还要照常发往远端。这两条缺任何一条，这个设计都不成立。
void TestMainWindowUi::clickingACellSelectsIt()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTest::qWait(50);

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    // 先把焦点挪到第 0 格，再点第 1 格
    w.viewGrid()->setSelectedIndex(0);
    QCOMPARE(w.selectedView(), w.viewAt(0));

    auto *renderer1 = w.viewAt(1)->renderer();
    // 画面区要先进入「采集输入」状态，否则它自己就不会发这条信号（见
    // overlayBarDoesNotForwardInputToRemote 的同一句）—— 这里要证明的是
    // 「选中没有把这颗按键吃掉」，不是在证明采集开关。
    renderer1->setInputEnabled(true);
    QSignalSpy buttons(renderer1, &lanrdp::ScreenRenderer::mouseButtonChanged);

    // ⚠️ y 要避开条（条占顶部 30 逻辑像素）—— 落在条上会被条自己吃掉（那是
    // 「点条 ≠ 点远端」），测的就不是画面区的这条路径了。
    const QPoint spot(20, 200);
    QTest::mousePress(renderer1, Qt::LeftButton, Qt::NoModifier, spot);
    QTest::mouseRelease(renderer1, Qt::LeftButton, Qt::NoModifier, spot);

    QCOMPARE(w.selectedView(), w.viewAt(1));   // 点一下这一格 -> 它变成选中格
    QVERIFY2(w.viewAt(1)->isSelected(), "被点的那一格应当带上强调线");
    QVERIFY2(!w.viewAt(0)->isSelected(), "没被点的那一格不该还亮着");
    QVERIFY2(buttons.count() >= 1,
             "点击不能被吞掉：画面区的按键仍要照常发往远端（选中只是旁听）");
}

// 上限 4：超出的请求只是**替换**，不会无限加格（也就不会退化成几十路的无底洞）。
void TestMainWindowUi::viewCountIsCappedAtFour()
{
    MainWindow w(testLogPath());
    QCOMPARE(w.maxViews(), 4);

    // 连一个刚空出来的端口：每次都会开一格（连不上但会排重连），直到上限。
    for (int i = 0; i < 6; ++i)
        w.startConnect(QStringLiteral("127.0.0.1"), pickDeadPort(), QStringLiteral("secret"));

    QCOMPARE(w.viewCount(), 4);
    QCOMPARE(w.viewGrid()->viewCount(), 4);
    QVERIFY2(!w.selectedView()->connectButton()->isEnabled(),
             "到格数上限后不应还能再加格");
}

// 切到被控模式：多余的控制格收起来 —— 被控端没有「多路画面」这回事，
// 留着旧格会让第 0 格之外还挂着别人的最后一帧。
// 被控不再收缩格子：开始 / 停止监听对**任何一格**都没有影响 ——
// 被控是全局功能，不是「换一种窗口布局」。
void TestMainWindowUi::hostModeCollapsesExtraCells()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    // 监听起停：两格原样保留（不再 collapse 到一格）
    w.startHost(0, QStringLiteral("secret"));
    QVERIFY2(w.isHosting(), "应当正在监听");
    QCOMPARE(w.viewCount(), 2);
    QCOMPARE(w.viewGrid()->viewCount(), 2);
    QVERIFY2(w.clientReady(), "已有连接不受影响");

    w.stopHost();
    QCOMPARE(w.viewCount(), 2);
    QVERIFY2(w.clientReady(), "停止监听同样不动格子");
}

// ---------------------------------------------------------------------------
// 占满 / 还原
//
// 「占满」把选中/被点的那一格铺满整个中央区、其余格 hide()（**不销毁** ——
// 帧照收，切回来立刻有画面）。入口是**每格条上最左**的那颗按钮；出口除了它，
// 占满态下 Esc 也还原（Esc 只在占满态本地化 —— 平时它是远端要用的键）。
// ---------------------------------------------------------------------------

// 只有一格时占满没有意义（格子本来就铺满），入口应当是「可见但置灰」，
// 而不是「点了没反应」。未选模式连入口都不该出现。
void TestMainWindowUi::maximizeButtonNeedsASecondCell()
{
    MainWindow w(testLogPath());

    QAbstractButton *btn = barButton(w, "maximizeButton");
    QVERIFY2(btn, "条上应当有【占满】按钮");
    QVERIFY2(!shown(btn), "未选模式不该出现占满入口（它属于控制端）");

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    QCOMPARE(w.viewCount(), 1);

    QVERIFY2(shown(w.viewAt(0)->maximizeButton()), "控制模式下占满入口应当可见");
    QVERIFY2(!w.viewAt(0)->maximizeButton()->isEnabled(),
             "只有一格时占满无从谈起，应当置灰");
    QVERIFY2(!w.viewGrid()->isMaximized(), "还没人要求占满");

    w.viewAt(0)->disconnectButton()->click();
}

// 占满的是**被点的那一格**（按格接线），其余格只 hide 不销毁，再点一次还原。
void TestMainWindowUi::maximizeFillsItsOwnCellAndRestores()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    QAbstractButton *btn0 = w.viewAt(0)->maximizeButton();
    QVERIFY(btn0 && btn0->isEnabled());

    // 这颗按钮**不带文字**（图标 + 悬停提示），所以「现在是哪一态」只能靠图形和
    // 提示语表达 —— 两样都得跟着状态变，否则用户没有任何线索。
    const auto barIcon = [](QAbstractButton *b) {
        return b->icon().pixmap(QSize(18, 18), 1.0, QIcon::Normal).toImage();
    };
    QVERIFY2(btn0->text().isEmpty(),
             "占满按钮只显示图标 —— 它右边还有四个按钮要挤在半格宽的条里");
    const QImage idleIcon = barIcon(btn0);
    QVERIFY2(!idleIcon.isNull(), "占满按钮上没画出图标");
    QVERIFY2(btn0->toolTip().contains(QStringLiteral("铺满")),
             "没有文字就得靠悬停提示说清这颗按钮做什么");

    btn0->click();
    QCOMPARE(w.viewGrid()->maximizedIndex(), 0);
    QVERIFY(w.viewGrid()->isMaximized());
    QVERIFY(w.viewAt(0)->isMaximized());
    QVERIFY2(!w.viewAt(0)->isHidden(), "占满的那一格要显示");
    QVERIFY2(w.viewAt(1)->isHidden(), "占满时其余格 hide()");
    QVERIFY2(w.viewAt(1)->connection() && w.viewAt(1)->connection()->ready(),
             "隐藏的格子连接不断（不是销毁）");
    QCOMPARE(hostB.sessions().size(), 1);
    QVERIFY2(barIcon(btn0) != idleIcon, "占满后图标要换成「收回来」的箭头");
    QVERIFY2(btn0->toolTip().contains(QStringLiteral("退出占满")),
             "占满后提示要变成出口的说法");

    // 第 1 格自己的按钮也能把**它自己**占满（不是「跟随选中」）
    w.viewAt(1)->maximizeButton()->click();
    QCOMPARE(w.viewGrid()->maximizedIndex(), 1);
    QVERIFY(w.viewAt(1)->isMaximized());
    QVERIFY2(w.viewAt(1)->maximizeButton()->toolTip().contains(QStringLiteral("退出占满"))
                 && w.viewAt(0)->maximizeButton()->toolTip().contains(QStringLiteral("铺满")),
             "按钮状态只反映自己那一格");

    w.viewAt(1)->maximizeButton()->click();
    QVERIFY2(!w.viewGrid()->isMaximized(), "再点一次应当还原");
    QVERIFY2(!w.viewAt(0)->isHidden() && !w.viewAt(1)->isHidden(),
             "还原后两格都要显示");
    QVERIFY2(barIcon(w.viewAt(1)->maximizeButton()) == idleIcon,
             "还原后图标要转回「朝外」那一版");

    w.viewAt(0)->disconnectButton()->click();
    w.viewAt(1)->disconnectButton()->click();
}

// 占满态下 Esc 是**本地**手势：既还原，又不发往远端。
// 两条缺一不可 —— 只还原不拦，远端会收到一颗它没期待的 Esc；
// 只拦不还原，用户就没有键盘出口（画面被占满时鼠标可能够不到别处）。
void TestMainWindowUi::escapeRestoresFromMaximized()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QTest::qWait(50);

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    w.viewAt(0)->maximizeButton()->click();
    QVERIFY(w.viewGrid()->isMaximized());

    auto *r0 = w.viewAt(0)->renderer();
    // 与 clickingACellSelectsIt 同一前提：不开采集，渲染器自己就不发 keyChanged，
    // 「Esc 没被转发」这件事就无从证明。
    r0->setInputEnabled(true);
    QSignalSpy keys(r0, &lanrdp::ScreenRenderer::keyChanged);

    QTest::keyPress(r0, Qt::Key_Escape);

    QVERIFY2(!w.viewGrid()->isMaximized(), "占满态下 Esc 应当本地还原");
    QCOMPARE(keys.count(), 0);

    // 还原之后 Esc 回到「发给远端」的老路（不能被本地永久吞掉）
    QTest::keyPress(r0, Qt::Key_Escape);
    QCOMPARE(keys.count(), 1);

    w.viewAt(0)->disconnectButton()->click();
    w.viewAt(1)->disconnectButton()->click();
}

// 双击**本格的条标题** = 占满 / 还原。
//
// 为什么是条标题而不是画面：画面上的双击属于远端（远程双击是最常用的操作之一），
// 本地抢不了。条本来就是「本地手势区」，且它会把事件吃掉，不会漏到画面控件。
void TestMainWindowUi::barDoubleClickTogglesMaximize()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    lanrdp::RemoteView *const v0  = w.viewAt(0);
    lanrdp::ScreenOverlayBar *bar = v0->bar();
    QVERIFY(bar);
    const QPoint spot(20, bar->height() / 2);   // 标题区（按钮都是子控件，事件到不了条）

    QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, spot);
    QTRY_VERIFY2(w.viewGrid()->isMaximized(), "双击条标题应当占满这一格");
    QCOMPARE(w.viewGrid()->maximizedIndex(), 0);

    QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, spot);
    QTRY_VERIFY2(!w.viewGrid()->isMaximized(), "再双击一次应当还原");

    v0->disconnectButton()->click();
    w.viewAt(1)->disconnectButton()->click();
}

// 点【占满】之后，**不点画面**也能按 Esc 退出。
//
// 真机手感问题：Esc 的拦截只装在画面控件上，而点完按钮焦点落在按钮上 ⇒ 用户按
// Esc 毫无反应，只能再去找那颗【还原】。占满时把焦点交给画面，键盘立刻能用；
// 条上按钮本身也不进焦点链（见 barButtonsNeverTakeFocus），焦点不会再被搬走。
void TestMainWindowUi::maximizeMovesFocusSoEscapeWorks()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    w.activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    lanrdp::RemoteView *const v0 = w.viewAt(0);

    // 条上按钮不进焦点链：点它们不会把焦点从画面上搬走（这条是前提，见
    // barButtonsNeverTakeFocus；setFocus() 对 NoFocus 控件无效，所以不能再用
    // 「先让焦点落在按钮上」来构造场景）。
    QVERIFY2(v0->maximizeButton()->focusPolicy() == Qt::NoFocus,
             "前置：条上按钮不该接受键盘焦点");
    v0->renderer()->setFocus();
    v0->maximizeButton()->click();
    QTRY_VERIFY(w.viewGrid()->isMaximized());

    QVERIFY2(QApplication::focusWidget() == v0->renderer(),
             "占满之后键盘焦点应当在画面上，否则 Esc 收不到");

    QTest::keyPress(v0->renderer(), Qt::Key_Escape);
    QTRY_VERIFY2(!w.viewGrid()->isMaximized(), "占满态下 Esc 应当直接还原");

    v0->disconnectButton()->click();
    w.viewAt(1)->disconnectButton()->click();
}

// 条上按钮一律不进键盘焦点链：焦点必须留在画面上。
// 否则点一下【让出操作权】之类的按钮，焦点就悄悄落到按钮上 —— 占满态唯一的键盘
// 出口是 Esc，而它的拦截只装在画面控件上，于是按 Esc 毫无反应。
void TestMainWindowUi::barButtonsNeverTakeFocus()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    lanrdp::HostServer host(QStringLiteral("secret"));
    QVERIFY(host.listen(0));
    w.startConnect(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());

    lanrdp::RemoteView *const v0 = w.viewAt(0);

    const QList<QToolButton *> buttons = v0->findChildren<QToolButton *>();
    QVERIFY2(!buttons.isEmpty(), "前置：这一格的条上应当有一排按钮");
    for (QToolButton *b : buttons) {
        QVERIFY2(b->focusPolicy() == Qt::NoFocus,
                 qPrintable(QStringLiteral("条上按钮 %1 不该接受键盘焦点，否则会吃掉占满态的 Esc")
                                .arg(b->objectName())));
    }

    // 点一颗按钮之后焦点仍要留在画面上 —— 这是「占满态 Esc 一定能用」的实际前提。
    v0->renderer()->setFocus();
    v0->disconnectButton()->click();
    QVERIFY2(QApplication::focusWidget() == v0->renderer(),
             "点条上按钮之后焦点不该离开画面");

    v0->disconnectButton()->click();
}

// 一格独享（或占满）时，渲染器**允许在死区也开平滑**。
//
// 死区关平滑是为了省 4 路 4K 的绘制开销；但只有一格要画时，那个省出来的 CPU
// 没人需要，反而让 2560→1100 这种常见缩小退化成最近邻、文字发虚（真机反馈）。
// 所以「是否允许死区平滑」要跟**可见格数**走，而不是写死成全局开关。
void TestMainWindowUi::singleCellUsesQualityScaling()
{
    MainWindow w(testLogPath());

    // 起步只有一格：允许死区平滑
    QVERIFY2(w.viewAt(0)->renderer()->smoothInDeadZone(),
             "一格独享时应允许死区平滑");

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    // 两格并排：回到省性能路径
    QVERIFY2(!w.viewAt(0)->renderer()->smoothInDeadZone(),
             "多格时应保持省性能（死区关平滑）");

    // 占满其中一格：又是只有一个可见 ⇒ 恢复死区平滑
    w.viewAt(0)->maximizeButton()->click();
    QTRY_VERIFY(w.viewGrid()->isMaximized());
    QVERIFY2(w.viewAt(0)->renderer()->smoothInDeadZone(),
             "占满后只剩一格可见，应恢复死区平滑");

    w.viewAt(0)->disconnectButton()->click();
    w.viewAt(1)->disconnectButton()->click();
}

// 占满时再点【新连接…】：必须**先退出占满** —— 否则新格一出生就是隐藏的，
// 用户点了「新连接」却看不到新格子，只能怀疑自己没连上。
void TestMainWindowUi::addingACellLeavesMaximizedState()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    lanrdp::HostServer hostC(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));
    QVERIFY(hostC.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    w.viewAt(0)->maximizeButton()->click();
    QVERIFY(w.viewGrid()->isMaximized());

    w.startConnect(QStringLiteral("127.0.0.1"), hostC.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 3);
    QTRY_VERIFY(w.viewAt(2)->connection() && w.viewAt(2)->connection()->ready());

    QVERIFY2(!w.viewGrid()->isMaximized(), "加新格必须先退出占满");
    QVERIFY2(!w.viewAt(2)->isHidden(), "新格要看得见");
    QVERIFY2(!w.viewAt(0)->isHidden() && !w.viewAt(1)->isHidden(),
             "退占满后原来那两格也要回来");

    for (int i = 0; i < w.viewCount(); ++i)
        w.viewAt(i)->disconnectButton()->click();
}

// ---------------------------------------------------------------------------
// 断线格的表现
//
// 断线的那一格**不清画面** —— 停在最后一帧并灰显（用户还能看清远端断线前长什么样），
// 条上同时给出两个出口：【重连】（立刻用记下的参数再试一次）与【关闭】（扔掉这一格）。
// 关键约束是「**只动那一格**」：其余格的连接、画面、状态一律不受影响
// —— `6a12131`（重连待命时点【断开】界面不复位）在多机下的变体就是「整体复位」。
// ---------------------------------------------------------------------------

// 断线格保留最后一帧 + 灰显 + 两个出口；活着的格子一样都不该有。
void TestMainWindowUi::disconnectedCellKeepsItsLastFrame()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    // 造一帧「最后一帧」：直接喂给渲染器。这条用例要证的是「断线时 UI 不清画面」
    // 这条约定，不是解码链路（那是 test_clientconnection_h264 的事）。
    auto *r0 = w.viewAt(0)->renderer();
    r0->setFrame(QImage(64, 48, QImage::Format_ARGB32));
    QVERIFY(r0->hasFrame());
    QVERIFY2(!r0->isStale(), "还有画面在收的时候不该是灰的");

    // 断线前：两个出口都不该出现
    QVERIFY2(!shown(w.viewAt(0)->retryButton()), "活着的格子不该出现【重连】");
    QVERIFY2(!shown(w.viewAt(0)->closeCellButton()), "活着的格子不该出现【关闭】");

    w.viewAt(0)->disconnectButton()->click();
    QTRY_VERIFY(!w.viewAt(0)->connection()->connected());

    QVERIFY2(r0->hasFrame(), "断线后要保留最后一帧（不清画面）");
    QVERIFY2(r0->isStale(), "最后一帧要按「已过期」画（灰显 + 提示），不能看着像活画面");
    QVERIFY2(!w.viewAt(1)->renderer()->isStale(), "另一格没断，不该被连坐");

    QVERIFY2(shown(w.viewAt(0)->retryButton()), "断线格要给【重连】");
    QVERIFY2(shown(w.viewAt(0)->closeCellButton()), "断线格要给【关闭】");
    QVERIFY2(w.viewAt(0)->title().contains(QStringLiteral("已断开")),
             "条标题要写明它断了（否则用户只看得到停住的画面）");

    // 只动那一格：另一格照旧
    QVERIFY2(w.viewAt(1)->connection() && w.viewAt(1)->connection()->ready(),
             "断一格不该把另一格也复位");
    QCOMPARE(hostB.sessions().size(), 1);

    w.viewAt(1)->disconnectButton()->click();
}

// 【重连】= 用记下的参数立刻再试一次（不等退避计时）。
void TestMainWindowUi::retryButtonReconnectsThatCell()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    w.viewAt(0)->renderer()->setFrame(QImage(64, 48, QImage::Format_ARGB32));
    w.viewAt(0)->disconnectButton()->click();
    QTRY_VERIFY(!w.viewAt(0)->connection()->connected());
    QTRY_COMPARE(hostA.sessions().size(), 0);

    w.viewAt(0)->retryButton()->click();

    QTRY_VERIFY(w.viewAt(0)->connection()->ready());
    QCOMPARE(hostA.sessions().size(), 1);
    QVERIFY2(!w.viewAt(0)->renderer()->isStale(), "重新连上后不该还是灰的");
    QVERIFY2(!shown(w.viewAt(0)->retryButton()), "连上了就不该再显示【重连】");
    QVERIFY2(!shown(w.viewAt(0)->closeCellButton()), "连上了就不该再显示【关闭】");
    QVERIFY2(w.viewAt(1)->connection() && w.viewAt(1)->connection()->ready(),
             "重连第 0 格不该动第 1 格");

    w.viewAt(0)->disconnectButton()->click();
    w.viewAt(1)->disconnectButton()->click();
}

// 【关闭】= 扔掉这一格（多格时）。留下的必须是**别的那一台**，不是「整体复位」。
void TestMainWindowUi::closeButtonDropsOnlyThatCell()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    // 记下「第 1 格是哪一台」——关掉第 0 格之后，剩下的必须是它
    const quint16 remaining = w.viewAt(1)->connection()->port();

    w.viewAt(0)->disconnectButton()->click();
    QTRY_VERIFY(!w.viewAt(0)->connection()->connected());

    w.viewAt(0)->closeCellButton()->click();

    QCOMPARE(w.viewCount(), 1);
    QCOMPARE(w.viewGrid()->viewCount(), 1);
    QVERIFY2(w.viewAt(0)->connection() && w.viewAt(0)->connection()->ready(),
             "留下的那一格必须还连着（关一格 ≠ 整体复位）");
    QCOMPARE(w.viewAt(0)->connection()->port(), remaining);
    QCOMPARE(hostB.sessions().size(), 1);
    QVERIFY(w.selectedView() == w.viewAt(0));

    w.viewAt(0)->disconnectButton()->click();
}

// 【关闭】改的是「名单」，而名单中间少一格会让后面的**全部前移** ——
// 选中的那格如果不跟着「格」走、只跟着下标走，关掉一台就会把「操作目标」悄悄
// 换到另一台上去（用户会觉得自己点错了）。这条钉住两种情形：
// · 关掉的**不是**选中格 ⇒ 选中必须留在原来那台上；
// · 关掉的**正是**选中格 ⇒ 才挪到旁边那一格。
void TestMainWindowUi::closeButtonKeepsSelectionOnTheUntouchedCell()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    lanrdp::HostServer hostC(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));
    QVERIFY(hostC.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostC.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 3);
    QTRY_VERIFY(w.clientReady());

    // 新连上的那一格自动成为焦点 ⇒ 此刻选中的是第 3 格
    lanrdp::RemoteView *const third = w.viewAt(2);
    QCOMPARE(w.selectedView(), third);

    // 断掉第 1 格（**没被选中**的那台）再关闭它
    w.viewAt(0)->disconnectButton()->click();
    QTRY_VERIFY(!w.viewAt(0)->connection()->connected());
    w.viewAt(0)->closeCellButton()->click();

    QCOMPARE(w.viewCount(), 2);
    QVERIFY2(w.selectedView() == third,
             "关掉的不是选中格，选中不该漂到别的台上去");
    QVERIFY(w.viewGrid()->viewAt(w.viewGrid()->selectedIndex()) == third);

    // 再关掉**正是选中**的那一格：这时才该挪到旁边（剩下的第 1 格）
    w.viewAt(1)->closeCellButton()->click();

    QCOMPARE(w.viewCount(), 1);
    QVERIFY(w.selectedView() == w.viewAt(0));
    QVERIFY2(w.viewAt(0)->connection() && w.viewAt(0)->connection()->ready(),
             "留下的那一格必须还连着");
    QCOMPARE(hostB.sessions().size(), 1);

    w.viewAt(0)->disconnectButton()->click();
}

// 只剩一格时【关闭】删不掉「格」本身（中央区总得有东西）——
// 语义退化成「扔掉这条连接、把这一格复位成空态」，而不是留一个断线格。
void TestMainWindowUi::closingTheOnlyCellResetsIt()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    QCOMPARE(w.viewCount(), 1);

    w.viewAt(0)->renderer()->setFrame(QImage(64, 48, QImage::Format_ARGB32));
    w.viewAt(0)->disconnectButton()->click();
    QTRY_VERIFY(!w.viewAt(0)->connection()->connected());
    QVERIFY(w.viewAt(0)->renderer()->hasFrame());

    w.viewAt(0)->closeCellButton()->click();

    QCOMPARE(w.viewCount(), 1);
    QVERIFY2(!w.viewAt(0)->connection(), "关闭唯一一格 = 扔掉这条连接（复位成空态）");
    QVERIFY2(!w.viewAt(0)->renderer()->hasFrame(), "复位要清掉那最后一帧，别留残影");
    QVERIFY2(!w.viewAt(0)->renderer()->isStale(), "空态不是断线态");
    QVERIFY2(!shown(w.viewAt(0)->retryButton()), "没有连接可重连了");
    QVERIFY2(!shown(w.viewAt(0)->closeCellButton()), "没有格可关了");
    QVERIFY2(w.viewAt(0)->connectButton()->isEnabled(), "复位后应当能再点【新连接…】");
}

// 关掉一格之后，**留下的那一格必须重新铺满**整个画面区。
//
// 真机报的现象：1 格 → 连第 2 台 → 关掉其中一格，界面看上去还是两格
//（留下那格只占左半边，右半边空着）。根因不是「没删」，而是网格的**列伸缩**
// 没清干净：把控件全摘掉之后 `QGridLayout::columnCount()` 已经归零，
// 「按 columnCount 归零伸缩」那圈循环等于什么都没做 ⇒ 上一套「2 列各 1 份」
// 的伸缩原样留着，唯一那一列只拿到一半宽度。
void TestMainWindowUi::closingACellExpandsTheRemainingOne()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    const int fullWidth = w.viewGrid()->width();
    QTRY_VERIFY2(w.viewAt(0)->width() > 0 && w.viewAt(0)->width() < fullWidth,
                 "前提：两格并排时每格都不该是满宽");

    // 【关闭】只在断线态出现：先断开，再关掉这一格
    w.viewAt(0)->disconnectButton()->click();
    QTRY_VERIFY(!w.viewAt(0)->connection()->connected());
    w.viewAt(0)->closeCellButton()->click();

    QCOMPARE(w.viewCount(), 1);
    QTRY_COMPARE(w.viewAt(0)->width(), w.viewGrid()->width());
    // 格子里的画面控件也要跟着长满 —— 只断言格子尺寸会漏掉「格满了、画面还是半屏」
    QTRY_COMPARE(w.viewAt(0)->renderer()->width(), w.viewAt(0)->width());

    w.viewAt(0)->disconnectButton()->click();
}

// 断线的那一格：条上**只该剩出口**。
//
// 真机报的现象：两格时断开其中一格，那一格收不起来 —— 界面一直是两格。
// 根因是条上的按钮按模式刷，**不按连接状态刷**：断线格上【发送文件】【让出操作权】
// 【断开】这些已经无意义的按钮还在，把条挤满，右端的【重连】【关闭】被挤出可见区域，
// 用户根本点不到（按钮还在，只是不在条上）。
void TestMainWindowUi::disconnectedCellOnlyOffersTheWayOut()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.viewCount() == 2);
    QTRY_VERIFY(w.clientReady());

    lanrdp::RemoteView *const v = w.viewAt(0);
    // 连着的时候，条上最后一个按钮也得在条内 —— 否则「加第 3 格」就点不到了
    QVERIFY2(v->connectButton()->x() + v->connectButton()->width() <= v->bar()->width(),
             qPrintable(QStringLiteral("【新连接…】跑到条外：x=%1 w=%2 条宽=%3")
                            .arg(v->connectButton()->x()).arg(v->connectButton()->width())
                            .arg(v->bar()->width())));
    v->disconnectButton()->click();
    QTRY_VERIFY(!v->connection()->connected());
    QTRY_VERIFY(shown(v->closeCellButton()));

    QVERIFY2(!shown(v->sendFileButton()), "断线格不该还摆着【发送文件】");
    QVERIFY2(!shown(v->releaseTokenButton()), "断线格不该还摆着【让出操作权】");
    QVERIFY2(!shown(v->disconnectButton()), "断线格不该还摆着【断开】");

    // 出口必须真的在条内 —— 被挤到条外等于不存在
    auto *close = v->closeCellButton();
    QVERIFY2(close->x() >= 0 && close->x() + close->width() <= v->bar()->width(),
             qPrintable(QStringLiteral("【关闭】跑到条外了：x=%1 w=%2 条宽=%3")
                            .arg(close->x()).arg(close->width()).arg(v->bar()->width())));

    v->closeCellButton()->click();
    QCOMPARE(w.viewCount(), 1);
    w.viewAt(0)->disconnectButton()->click();
}

// 验收时抓到的真 bug —— 画面没铺满自己的格子。
// 现象（真机截图量出来的）：窗口内容区 1662 px 宽，而画面（深色区）只有 **485 px**
// = 窗口的 29%。485 / DPR(1.5) ≈ **323 逻辑 px** —— 正好是 `ScreenRenderer` 的
// `setMinimumSize(320, 240)`：renderer 停在最小值上，根本没被拉大。
//
// 根因：`RemoteView` 把 `renderer_` 建成了**直接子控件**，但本格**没有任何布局**
// （条挂 renderer 上、renderer 挂本格上，三者是「裸嵌套」）。Qt 里裸子控件不会
// 跟着父控件 resize —— `Expanding` 的 sizePolicy 只在**布局**里才起作用。
//
// 为什么之前没暴露：`test_viewgrid` 只断言落位（行列）不断尺寸，`test_mainwindow_ui`
// 也没有一条断言「画面尺寸 == 格子尺寸」。而 `dist/` 长期停在**单渲染器版**（单
// renderer 直接当 centralWidget，有主窗口布局撑着），所以直到多视窗版第一次拿真机
// 截图才看见。⇒ 教训：**布局类的「铺满」必须有一条断言**。
void TestMainWindowUi::rendererFillsItsCell()
{
    MainWindow w(testLogPath());
    w.resize(1100, 760);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto *v = w.viewAt(0);
    QVERIFY(v);

    // 布局落定要等事件循环（show 之后几何才算数）。
    QTRY_VERIFY(v->width() > 200 && v->height() > 200);

    const QSize cell = v->size();
    const QSize scr  = v->renderer()->size();

    QVERIFY2(scr.width() == cell.width(),
             qPrintable(QStringLiteral("画面宽度 %1 应当等于格子宽度 %2")
                            .arg(scr.width()).arg(cell.width())));
    QVERIFY2(scr.height() == cell.height(),
             qPrintable(QStringLiteral("画面高度 %1 应当等于格子高度 %2")
                            .arg(scr.height()).arg(cell.height())));

    // 反向断言：别让它退化成 minimumSize（320x240）—— 那正是这个 bug 的表征。
    QVERIFY2(scr.width() > 320 + 8,
             qPrintable(QStringLiteral("画面宽度 %1 贴着 minimumSize(320)，说明没被拉大")
                            .arg(scr.width())));
}

// 同一件事在多格 + 重排之后仍要成立：切布局（1 -> 2 格）会让每格变窄，
// 画面必须跟着变 —— 这条覆盖「relayout 之后 renderer 会不会脱队」。
void TestMainWindowUi::rendererKeepsFillingAfterRelayout()
{
    MainWindow w(testLogPath());
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    const int before = w.viewAt(0)->renderer()->width();   // 1 格时较宽

    lanrdp::HostServer hostA(QStringLiteral("secret"));
    lanrdp::HostServer hostB(QStringLiteral("secret"));
    QVERIFY(hostA.listen(0));
    QVERIFY(hostB.listen(0));

    // 第 0 格是空的预览格，第一台连上会**复用它**；要变成两格得连第二台。
    w.startConnect(QStringLiteral("127.0.0.1"), hostA.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    w.startConnect(QStringLiteral("127.0.0.1"), hostB.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());
    QTRY_COMPARE(w.viewCount(), 2);

    // 两格并排 => 每格约半宽，画面要跟着变窄。
    QTRY_VERIFY(w.viewAt(0)->renderer()->width() < before);

    for (int i = 0; i < w.viewCount(); ++i) {
        auto *v = w.viewAt(i);
        if (!v || v->isHidden())
            continue;   // 占满态下被隐藏的格不参与（尺寸由布局决定，不是这次的考点）
        QCOMPARE(v->renderer()->size(), v->size());
    }
}

// ---------------------------------------------------------------------------
// 输入路径加固
// ---------------------------------------------------------------------------

// 未勾选「启用远程键鼠控制」时输入会被**静默丢弃** —— 用户看到「已获得输入权」
// 却在画面上点什么都没反应。修法：画面中央给一层可见提示，点它即开启。
void TestMainWindowUi::disabledInputShowsClickableHint()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer host(QStringLiteral("secret"));
    QVERIFY(host.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());

    auto *view = w.viewAt(0);
    QVERIFY(view);
    auto *renderer = view->renderer();
    QVERIFY(renderer);

    // 给一点画面 —— 提示只在**有画面**时显示（空态已有自己的文案，叠起来会打架）
    QImage image(320, 240, QImage::Format_ARGB32);
    image.fill(Qt::black);
    renderer->setFrame(image);
    renderer->resize(320, 240);

    // 默认关着 —— 提示必须出现
    QVERIFY2(!renderer->inputEnabled(), "键鼠开关默认应当是关的");
    QVERIFY2(renderer->inputHintVisible(),
             "键鼠未启用时应当有可见提示（否则输入被静默丢弃）");

    // 有画面才画提示：空态本来就有 idleText_，再叠一层会打架
    renderer->clear();
    QVERIFY2(!renderer->inputHintVisible(), "没有画面时不该画键鼠提示（会与空态文案重叠）");
}

void TestMainWindowUi::clickingHintEnablesInput()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer host(QStringLiteral("secret"));
    QVERIFY(host.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());

    auto *view = w.viewAt(0);
    QVERIFY(view);
    auto *renderer = view->renderer();

    // 给一点画面，让提示有资格显示
    QImage image(320, 240, QImage::Format_ARGB32);
    image.fill(Qt::black);
    renderer->setFrame(image);
    renderer->resize(320, 240);
    QVERIFY(renderer->inputHintVisible());
    QVERIFY(!renderer->inputEnabled());

    QSignalSpy requested(renderer, &lanrdp::ScreenRenderer::inputEnableRequested);

    // 在提示上点一下（画面中央）
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(160, 120), QPointF(160, 120),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(renderer, &press);

    QCOMPARE(requested.count(), 1);

    // 点它 = 等价于【设置…】里勾上「启用远程键鼠控制」
    QTRY_VERIFY(renderer->inputEnabled());
    QTRY_VERIFY(view->connection() && view->connection()->inputEnabled());

    // 这一下点击**不该**同时被当成远端输入（它是本地手势）
    // —— 通过开关已翻转（说明走了开启路径而不是采集路径）来间接保证。
    QVERIFY2(renderer->inputHintVisible() == false, "开启后提示应当消失");
}

// 「按住键 → 打断（关开关）→ 远端收到抬起」完整链条。
// 状态机的「任意中间态被打断」用例。
// 症状：按住 Ctrl 时关掉键鼠开关 ⇒ 远端 Ctrl **永久按下**，整机基本不可用。
void TestMainWindowUi::turningOffSwitchReleasesPressedKey()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer host(QStringLiteral("secret"));
    QVERIFY(host.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());

    auto *view = w.viewAt(0);
    QVERIFY(view);
    auto *renderer = view->renderer();
    QVERIFY(renderer);

    // 手动开开关（等价于【设置…】勾上），并把开关注入到这一格
    w.setInputEnabledForTest(true);
    QTRY_VERIFY(renderer->inputEnabled());

    QImage image(320, 240, QImage::Format_ARGB32);
    image.fill(Qt::black);
    renderer->setFrame(image);
    renderer->resize(320, 240);

    // 按住 Ctrl
    QSignalSpy keys(renderer, &lanrdp::ScreenRenderer::keyChanged);
    QKeyEvent  press(QEvent::KeyPress, Qt::Key_Control, Qt::NoModifier);
    QCoreApplication::sendEvent(renderer, &press);
    QCOMPARE(keys.count(), 1);
    QCOMPARE(keys.at(0).at(1).toBool(), true);
    keys.clear();

    // 打断：关掉总开关
    w.setInputEnabledForTest(false);

    // 必须补发一次抬起 —— 否则远端那个键永久按下
    QTRY_COMPARE(keys.count(), 1);
    QCOMPARE(keys.at(0).at(1).toBool(), false);
    QCOMPARE(renderer->inputEnabled(), false);
}

// 被控端同样不能对心跳「收到就回」—— 两端都回会形成无限乒乓，
// 把 3 s 一次的轻量心跳变成帧风暴（真机实测 42 s 收到 56711 帧，
// 被控端 GUI 线程 CPU 从 35% 涨到 82%，进而饿死心跳与新连接应答）。
//
// 判据用「对端发 N 帧、被控端回几帧」：一帧未回才算对（旧实现一帧对一帧地回）。
void TestMainWindowUi::hostDoesNotEchoHeartbeat()
{
    MainWindow host(testLogPath());
    quint16    port = 0;
    QObject::connect(&host, &MainWindow::listening, &host,
                     [&port](quint16 p) { port = p; });
    host.startHost(0, QStringLiteral("secret"));
    QTRY_VERIFY(port != 0);

    // 原始对端（老客户端行为）：完成 v2 握手后，收到 Heartbeat 立刻回一帧。
    auto *sock = new QTcpSocket();
    sock->connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(sock->waitForConnected(3000));

    auto       *session = new lanrdp::Session(sock);
    auto       *auth    = new lanrdp::AuthClient(session, QStringLiteral("secret"));
    bool        authed  = false;
    QObject::connect(auth, &lanrdp::AuthClient::succeeded, session,
                     [&authed]() { authed = true; });
    auth->start();
    QTRY_VERIFY2(authed, "对端应当通过鉴权（被控端的收帧处理只在鉴权后挂上）");

    int hbFromHost = 0;
    QObject::connect(session, &lanrdp::Session::frameReceived, session,
                     [&hbFromHost](const lanrdp::Frame &f) {
                         if (f.type == static_cast<std::uint8_t>(lanrdp::FrameType::Heartbeat))
                             ++hbFromHost;
                     });

    // 连发 8 帧心跳（每帧之间让事件循环跑一拍，确保被控端真的读到）。
    // 用「显式连发」而不是靠自持风暴：一帧对一帧的对应关系更直观，也不依赖
    // 「一帧引出的回帧还能再引出下一帧」这种链式行为。
    for (int i = 0; i < 8; ++i) {
        session->sendFrame(static_cast<std::uint8_t>(lanrdp::FrameType::Heartbeat));
        QCoreApplication::processEvents();
    }
    QTest::qWait(200);

    // 老实现一帧对一帧地回（≈8 帧）；新实现一帧都不回。
    // 被控端自己那份定时器是 3 s ⇒ 这点时间里最多蹭到 1 个 tick。
    QVERIFY2(hbFromHost < 3,
             qPrintable(QStringLiteral("被控端回了 %1 帧心跳 —— 心跳正在乒乓").arg(hbFromHost)));

    delete session;   // 连带释放 socket 与 auth
}

// 【新连接…】地址为空时必须在**对话框内**拦下：关了窗也必然连不上，
// 不如当场把焦点打回输入框说清楚。
//
// 正反两面都要验 —— 只断言「空地址时没关窗」的话，一个永不 accept 的实现
//（比如 accept() 被写成了空函数）也照样绿。
void TestMainWindowUi::connectDialogRejectsEmptyHost()
{
    lanrdp::ConnectDialog::Config cfg;
    cfg.host = QString();   // 空地址

    lanrdp::ConnectDialog dlg(cfg);
    dlg.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dlg));

    dlg.accept();
    QVERIFY2(dlg.isVisible(), "地址为空时不该关掉对话框");
    QCOMPARE(dlg.result(), int(QDialog::Rejected));   // 没走 QDialog::accept()

    // 反面：填上合法地址**和合规口令**后再点，必须真的能关
    //（地址合法不等于可以连 —— 口令是另一道校验，见 connectDialogRejectsWeakPassword）
    auto *hostEdit = dlg.findChild<QLineEdit *>(QStringLiteral("peerHostEdit"));
    QVERIFY2(hostEdit, "应当有对方地址输入框（objectName=peerHostEdit）");
    hostEdit->setText(QStringLiteral("192.168.1.10"));

    auto *pwEdit = dlg.findChild<QLineEdit *>(QStringLiteral("connectPasswordEdit"));
    QVERIFY2(pwEdit, "应当有口令输入框（objectName=connectPasswordEdit）");
    pwEdit->setText(QStringLiteral("abcde1"));

    dlg.accept();
    QVERIFY2(!dlg.isVisible(), "地址合法时应当关窗");
    QCOMPARE(dlg.result(), int(QDialog::Accepted));
}

// 上次用过的参数必须在**窗口刚建好**时就生效，而不是等用户再打开一次对话框。
// 用 hostUsesH264() 当观察点：它是这块私有状态的公开读法（编码方式来自同一份 ini）。
void TestMainWindowUi::restoresSavedSessionParameters()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::HostSession host;
    host.port       = 9333;
    host.password   = QStringLiteral("secret7");
    host.receiveDir = QStringLiteral("D:/tmp/received");
    host.useH264    = false;

    lanrdp::PeerSession peer;
    peer.host     = QStringLiteral("192.168.1.42");
    peer.port     = 9444;
    peer.password = QStringLiteral("secret7");

    const QString ini = QDir(dir.path()).absoluteFilePath(QStringLiteral("session.ini"));
    lanrdp::SessionStore store(ini);
    QVERIFY(store.saveHost(host));
    QVERIFY(store.savePeer(peer));

    MainWindow w(testLogPath(), ini);
    QVERIFY2(!w.hostUsesH264(), "构造时应当回填 ini 里的编码设置");

    // 口令没有公开读法，这里直接从 store 再读一次，确认它能被解回原文 ——
    // 窗口拿到的就是这一份（同一个 loadHost()）。
    QCOMPARE(store.loadHost().password, QStringLiteral("secret7"));
}

// 默认（不带路径）必须完全不碰文件系统。30+ 个 UI 用例都走这条路径，
// 一旦它们开始读写开发机上真实的 session.ini，失败会表现为互不相关的用例随机变红。
void TestMainWindowUi::ignoresSessionStoreWhenPathIsEmpty()
{
    MainWindow w(testLogPath());
    QVERIFY2(w.hostUsesH264(), "不接配置时应当保持内置默认值");
}

// ---- 多屏：条上的【屏幕】----

namespace {

lanrdp::ScreenInfo uiScreen(qint32 x, quint32 w, quint32 h, bool primary)
{
    lanrdp::ScreenInfo s;
    s.x       = x;
    s.width   = w;
    s.height  = h;
    s.primary = primary;
    return s;
}

lanrdp::ScreenList singleScreenList()
{
    lanrdp::ScreenList list;
    list.screens.append(uiScreen(0, 3840, 2160, true));
    list.current = 0;
    return list;
}

lanrdp::ScreenList dualScreenList(quint8 current = 1)
{
    lanrdp::ScreenList list;
    list.screens.append(uiScreen(0, 3840, 2160, true));
    list.screens.append(uiScreen(-1920, 1920, 1080, false));
    list.current = current;
    return list;
}

} // namespace

// 单屏被控端上，控制端界面必须与没有这项功能时**一模一样**：
// 多一颗按钮在窄格里会挤掉右侧出口（一控多 2×2 时每格只有半个窗口宽）。
void TestMainWindowUi::screenButtonAppearsOnlyWithMultipleScreens()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer host(QStringLiteral("secret"));
    QVERIFY(host.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());

    auto *view = w.viewAt(0);
    QVERIFY(view);

    // 还没收到屏列表 ⇒ 隐藏
    QVERIFY2(!shown(view->screenButton()), "没收到屏列表时不该出现【屏幕】");

    lanrdp::Session *peer = host.sessions().at(0);
    QVERIFY(peer);

    // 单屏 ⇒ 仍然隐藏
    peer->sendFrame(static_cast<quint8>(lanrdp::FrameType::ScreenList),
                    lanrdp::encodeScreenList(singleScreenList()));
    QTRY_VERIFY(view->connection()->hasScreenList());
    QTest::qWait(50);
    QVERIFY2(!shown(view->screenButton()), "只有一块屏时不该出现【屏幕】（界面零变化）");

    // 双屏 ⇒ 出现
    peer->sendFrame(static_cast<quint8>(lanrdp::FrameType::ScreenList),
                    lanrdp::encodeScreenList(dualScreenList()));
    QTRY_VERIFY_WITH_TIMEOUT(shown(view->screenButton()), 3000);
    QVERIFY2(!view->screenButton()->accessibleName().isEmpty(),
             "纯图标按钮必须有无障碍名字");
    QVERIFY2(!view->screenButton()->toolTip().isEmpty(), "纯图标按钮必须有悬停提示");
}

// 菜单项 = 屏列表现状：每块屏一条，当前那块打勾，点下去发的是**请求**。
// 抽成 createScreenMenu() 是为了能直接检查一个「没在模态弹出」的菜单。
void TestMainWindowUi::screenMenuListsEveryMonitorAndMarksCurrent()
{
    MainWindow w(testLogPath());

    lanrdp::HostServer host(QStringLiteral("secret"));
    QVERIFY(host.listen(0));

    w.startConnect(QStringLiteral("127.0.0.1"), host.port(), QStringLiteral("secret"));
    QTRY_VERIFY(w.clientReady());

    auto *view = w.viewAt(0);
    QVERIFY(view);
    lanrdp::Session *peer = host.sessions().at(0);
    QVERIFY(peer);

    peer->sendFrame(static_cast<quint8>(lanrdp::FrameType::ScreenList),
                    lanrdp::encodeScreenList(dualScreenList(/*current=*/1)));
    QTRY_VERIFY(view->connection()->hasScreenList());

    QMenu *menu = w.createScreenMenu(view);
    QVERIFY(menu != nullptr);

    const QList<QAction *> actions = menu->actions();
    QCOMPARE(actions.size(), 2);
    // 文案里必须能读出分辨率（两块屏长得一样时，序号是唯一可分辨的信息，
    // 但分辨率才是用户认屏的依据）
    QVERIFY2(actions.at(0)->text().contains(QStringLiteral("3840"))
                 && actions.at(0)->text().contains(QStringLiteral("2160")),
             qPrintable(actions.at(0)->text()));
    QVERIFY2(actions.at(1)->text().contains(QStringLiteral("1920"))
                 && actions.at(1)->text().contains(QStringLiteral("1080")),
             qPrintable(actions.at(1)->text()));
    // 当前那一块打勾
    QVERIFY2(!actions.at(0)->isChecked(), "当前是屏 2，屏 1 不该打勾");
    QVERIFY2(actions.at(1)->isChecked(), "当前是屏 2，必须打勾");

    // 点第一项 ⇒ 往对端发一次 SCREEN_SELECT(0)
    QVector<quint8> requested;
    QObject::connect(peer, &lanrdp::Session::frameReceived, peer,
                     [&requested](const lanrdp::Frame &f) {
                         if (f.type != static_cast<quint8>(lanrdp::FrameType::ScreenSelect))
                             return;
                         if (auto idx = lanrdp::decodeScreenSelect(f.payload))
                             requested.append(*idx);
                     });
    actions.at(0)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!requested.isEmpty(), 3000);
    QCOMPARE(requested.last(), quint8(0));

    delete menu;
}

QTEST_MAIN(TestMainWindowUi)
#include "test_mainwindow_ui.moc"
