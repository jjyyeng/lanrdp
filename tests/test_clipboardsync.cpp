#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include "net/ClipboardSync.h"
#include "protocol/Clipboard.h"
#include "protocol/Frame.h"

// ClipboardSync 的双向同步与回环抑制
//
// 假剪贴板端口：完全离线；setText 后发 changed，用来验证 ClipboardSync 的
// applyingRemote_ 标志确实生效。真机（部分平台）的 QClipboard 写入后 dataChanged
// 是**异步投递**的 —— 可能晚到 applyingRemote_ 已清掉之后，那时只能靠内容比对兜底，
// 故本端口可用 asyncDelivery 模拟该时序（见 asyncClipboardDeliveryStillSuppressed）。
class FakeClipboardPort : public lanrdp::ClipboardPort
{
    Q_OBJECT

public:
    using lanrdp::ClipboardPort::ClipboardPort;

    QString text() const override { return text_; }

    void setText(const QString &text) override
    {
        ++writeCount;
        text_ = text;
        if (asyncDelivery) {
            // 模拟「本轮事件循环之后」才投递的 dataChanged
            QTimer::singleShot(0, this, [this] { emit changed(); });
        } else {
            emit changed();
        }
    }

    // 模拟「本地用户复制了内容」：改内容 + 发 changed（始终同步）
    void userCopy(const QString &text)
    {
        text_ = text;
        files_.clear();
        emit changed();
    }

    // 模拟「在资源管理器里 Ctrl+C 了几个文件」
    void userCopyFiles(const QStringList &paths)
    {
        text_.clear();
        files_ = paths;
        emit changed();
    }

    QStringList files() const override { return files_; }

    void setFiles(const QStringList &paths) override
    {
        ++filesWriteCount;
        files_ = paths;
        if (asyncDelivery) {
            QTimer::singleShot(0, this, [this] { emit changed(); });
        } else {
            emit changed();
        }
    }

    int  writeCount     = 0;
    int  filesWriteCount = 0;
    bool asyncDelivery  = false;  // true 时 setText 的 changed 延到事件循环下一拍

private:
    QString     text_;
    QStringList files_;
};

class TestClipboardSync : public QObject
{
    Q_OBJECT

private slots:
    void localChangeIsSentOnce();
    void remoteFrameIsAppliedWithoutLoopback();
    void identicalRemoteTextIsIgnored();
    void oversizedTextIsRejected();
    void pausedWhenNoTarget();
    void asyncClipboardDeliveryStillSuppressed();

    // 文件剪贴板（v6）
    void copiedFilesBecomePendingWithoutSending();
    void discardPendingClearsTheButton();
    void incomingFilesAreAssembledThenPasted();
    void incompleteBatchIsDroppedOnTimeout();
    void strayFileDoesNotTouchClipboard();
    void mixedSelectionSendsOnlyFiles();
};

void TestClipboardSync::localChangeIsSentOnce()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    QVector<lanrdp::Frame> out;
    sync.setPort(&port);
    sync.setFrameSink([&out](const lanrdp::Frame &f) { out.append(f); });

    QSignalSpy sentSpy(&sync, &lanrdp::ClipboardSync::sent);

    port.userCopy(QStringLiteral("hello"));
    QCOMPARE(sentSpy.count(), 1);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out.last().type, static_cast<std::uint8_t>(lanrdp::FrameType::ClipboardText));

    const auto decoded = lanrdp::decodeClipboardText(out.last().payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->text, QStringLiteral("hello"));

    // 内容没变 -> 再发一次 changed 也不重复发送
    port.userCopy(QStringLiteral("hello"));
    QCOMPARE(sentSpy.count(), 1);

    // 内容变了 -> 再发一次
    port.userCopy(QStringLiteral("world"));
    QCOMPARE(sentSpy.count(), 2);

    const auto second = lanrdp::decodeClipboardText(out.last().payload);
    QVERIFY(second.has_value());
    QCOMPARE(second->text, QStringLiteral("world"));
}

void TestClipboardSync::remoteFrameIsAppliedWithoutLoopback()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    QVector<lanrdp::Frame> out;
    sync.setPort(&port);
    sync.setFrameSink([&out](const lanrdp::Frame &f) { out.append(f); });

    QSignalSpy sentSpy(&sync, &lanrdp::ClipboardSync::sent);
    QSignalSpy recvSpy(&sync, &lanrdp::ClipboardSync::received);

    lanrdp::Frame frame;
    frame.type    = static_cast<std::uint8_t>(lanrdp::FrameType::ClipboardText);
    frame.payload = *lanrdp::encodeClipboardText(QStringLiteral("world"));

    QVERIFY(sync.handleFrame(frame));  // 本类消费了该帧

    QCOMPARE(port.text(), QStringLiteral("world"));
    QCOMPARE(recvSpy.count(), 1);
    QCOMPARE(sentSpy.count(), 0);  // 关键：写入本地剪贴板没有回环成一次发送
    QVERIFY(out.isEmpty());
}

void TestClipboardSync::identicalRemoteTextIsIgnored()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    sync.setPort(&port);

    QSignalSpy recvSpy(&sync, &lanrdp::ClipboardSync::received);

    const auto makeFrame = [](const QString &t) {
        lanrdp::Frame f;
        f.type    = static_cast<std::uint8_t>(lanrdp::FrameType::ClipboardText);
        f.payload = *lanrdp::encodeClipboardText(t);
        return f;
    };

    QVERIFY(sync.handleFrame(makeFrame(QStringLiteral("same"))));
    QVERIFY(sync.handleFrame(makeFrame(QStringLiteral("same"))));

    QCOMPARE(recvSpy.count(), 1);  // 第二次不 emit
    QCOMPARE(port.writeCount, 1);  // 也不重复写入
}

void TestClipboardSync::oversizedTextIsRejected()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    QVector<lanrdp::Frame> out;
    sync.setPort(&port);
    sync.setFrameSink([&out](const lanrdp::Frame &f) { out.append(f); });

    QSignalSpy rejectSpy(&sync, &lanrdp::ClipboardSync::rejected);

    const QString huge(static_cast<int>(lanrdp::kClipboardMaxBytes) + 1, QLatin1Char('x'));
    port.userCopy(huge);

    QCOMPARE(rejectSpy.count(), 1);
    QVERIFY(out.isEmpty());  // 超限不发帧

    // 理由必须是给用户看的人话（它会原样进状态栏）：说清「上限」与「没同步」，
    // 不写内部常量名。判据是这两层意思都在，而不是某个具体字节数。
    const QString reason = rejectSpy.at(0).at(0).toString();
    QVERIFY2(reason.contains(QStringLiteral("上限")), qPrintable(reason));
    QVERIFY2(reason.contains(QStringLiteral("未同步")), qPrintable(reason));
}

void TestClipboardSync::pausedWhenNoTarget()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    sync.setPort(&port);

    QSignalSpy sentSpy(&sync, &lanrdp::ClipboardSync::sent);

    // 故意不设置同步目标（等价于未连接 / 无人持令牌）
    port.userCopy(QStringLiteral("paused"));
    QCOMPARE(sentSpy.count(), 0);

    // 目标接上后，内容再变一次即可正常同步
    QVector<lanrdp::Frame> out;
    sync.setFrameSink([&out](const lanrdp::Frame &f) { out.append(f); });
    port.userCopy(QStringLiteral("resumed"));
    QCOMPARE(out.size(), 1);

    const auto decoded = lanrdp::decodeClipboardText(out.last().payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->text, QStringLiteral("resumed"));
}

// 真机即便异步投递 dataChanged，也必须靠「内容比对」兜住回环 —— 光靠 applyingRemote_
// 不够：该标志在 handleFrame 返回时就清了，而 changed 可能更晚才到。
void TestClipboardSync::asyncClipboardDeliveryStillSuppressed()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    port.asyncDelivery = true;
    sync.setPort(&port);

    QVector<lanrdp::Frame> out;
    sync.setFrameSink([&out](const lanrdp::Frame &f) { out.append(f); });
    QSignalSpy sentSpy(&sync, &lanrdp::ClipboardSync::sent);

    lanrdp::Frame frame;
    frame.type    = static_cast<std::uint8_t>(lanrdp::FrameType::ClipboardText);
    frame.payload = *lanrdp::encodeClipboardText(QStringLiteral("async-world"));
    QVERIFY(sync.handleFrame(frame));

    // 把异步投递的 changed 放出来（此刻 applyingRemote_ 早已为 false）
    QTest::qWait(20);

    QCOMPARE(sentSpy.count(), 0);   // 关键：没有回环成一次发送
    QVERIFY(out.isEmpty());
    QCOMPARE(port.writeCount, 1);   // 也不重复写入
}

// 复制文件**不自动发**：只记成「待发」并通知界面。复制常常只是本机换个地方放，
// 自动发过去既白占带宽，又会把对方的剪贴板顶掉 —— 发不发由用户在条上点。
void TestClipboardSync::copiedFilesBecomePendingWithoutSending()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    sync.setPort(&port);

    QVector<lanrdp::Frame> out;
    sync.setFrameSink([&out](const lanrdp::Frame &f) { out.append(f); });

    QSignalSpy pendingSpy(&sync, &lanrdp::ClipboardSync::pendingFilesChanged);
    QVERIFY(sync.pendingLocalFiles().isEmpty());

    const QStringList paths{QStringLiteral("C:/tmp/报告.docx"), QStringLiteral("C:/tmp/b.txt")};
    port.userCopyFiles(paths);

    QCOMPARE(pendingSpy.count(), 1);
    QCOMPARE(sync.pendingLocalFiles(), paths);
    QVERIFY2(out.isEmpty(), "复制文件不该自动发出任何帧");

    // 同一批再触发一次 changed（真实剪贴板会重复通知）-> 不重复通知界面
    port.userCopyFiles(paths);
    QCOMPARE(pendingSpy.count(), 1);

    // 剪贴板换成文本：待发作废，按钮要熄灭
    port.userCopy(QStringLiteral("after"));
    QCOMPARE(pendingSpy.count(), 2);
    QVERIFY(sync.pendingLocalFiles().isEmpty());
}

// 发出去（或用户不打算发）之后要能清掉；而「同一批再复制一次」必须还能重新点亮 ——
// 否则发过一次之后就再也发不出去了（剪贴板里确实还装着那批文件）。
void TestClipboardSync::discardPendingClearsTheButton()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    sync.setPort(&port);

    const QStringList paths{QStringLiteral("C:/tmp/a.txt")};
    port.userCopyFiles(paths);
    QCOMPARE(sync.pendingLocalFiles(), paths);

    QSignalSpy pendingSpy(&sync, &lanrdp::ClipboardSync::pendingFilesChanged);
    sync.discardPendingLocalFiles();
    QVERIFY(sync.pendingLocalFiles().isEmpty());
    QCOMPARE(pendingSpy.count(), 1);   // 清空也要通知界面

    // 同一批再复制一次：照常点亮（这是「发给第二台」的唯一姿势）
    port.userCopyFiles(paths);
    QCOMPARE(sync.pendingLocalFiles(), paths);

    // 换一批新文件则照常点亮
    sync.discardPendingLocalFiles();
    port.userCopyFiles({QStringLiteral("C:/tmp/c.txt")});
    QCOMPARE(sync.pendingLocalFiles(),
             (QStringList{QStringLiteral("C:/tmp/c.txt")}));
}

// 收到清单 → 每落盘一个文件报一次 → 凑齐才写本地剪贴板（这就是「能粘贴」的全部）。
void TestClipboardSync::incomingFilesAreAssembledThenPasted()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    sync.setPort(&port);

    QVector<lanrdp::Frame> out;
    sync.setFrameSink([&out](const lanrdp::Frame &f) { out.append(f); });

    QSignalSpy incomingSpy(&sync, &lanrdp::ClipboardSync::filesIncoming);
    QSignalSpy receivedSpy(&sync, &lanrdp::ClipboardSync::filesReceived);

    lanrdp::Frame frame;
    frame.type    = static_cast<std::uint8_t>(lanrdp::FrameType::ClipboardFiles);
    frame.payload = *lanrdp::encodeClipboardFiles(
        {QStringLiteral("a.txt"), QStringLiteral("b.txt")});
    QVERIFY(sync.handleFrame(frame));
    QCOMPARE(incomingSpy.count(), 1);
    QCOMPARE(sync.pendingFileCount(), 2);

    sync.noteReceivedFile(QStringLiteral("C:/recv/a.txt"));
    QCOMPARE(sync.pendingFileCount(), 1);
    QCOMPARE(receivedSpy.count(), 0);
    QVERIFY(port.files().isEmpty());          // 还差一个，绝不先写半批

    sync.noteReceivedFile(QStringLiteral("C:/recv/b.txt"));
    QCOMPARE(sync.pendingFileCount(), 0);
    QCOMPARE(receivedSpy.count(), 1);
    QCOMPARE(port.files(), (QStringList{QStringLiteral("C:/recv/a.txt"),
                                        QStringLiteral("C:/recv/b.txt")}));

    // 写本地剪贴板不许回环成一次「发送」—— 否则两端会互相传同一批文件
    QCOMPARE(port.filesWriteCount, 1);
    QCOMPARE(out.size(), 0);
    // 也不许变成本机的「待发」：收到的文件不该在按钮上亮起来
    QVERIFY(sync.pendingLocalFiles().isEmpty());
}

// 差一个文件时不许无限等：超时后整批放弃，并说明原因（它会进状态栏）。
void TestClipboardSync::incompleteBatchIsDroppedOnTimeout()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    sync.setPort(&port);
    sync.setFileAssembleTimeoutMs(30);

    QSignalSpy rejectSpy(&sync, &lanrdp::ClipboardSync::rejected);
    QSignalSpy receivedSpy(&sync, &lanrdp::ClipboardSync::filesReceived);

    lanrdp::Frame frame;
    frame.type    = static_cast<std::uint8_t>(lanrdp::FrameType::ClipboardFiles);
    frame.payload = *lanrdp::encodeClipboardFiles(
        {QStringLiteral("a.txt"), QStringLiteral("b.txt")});
    QVERIFY(sync.handleFrame(frame));

    sync.noteReceivedFile(QStringLiteral("C:/recv/a.txt"));
    QTest::qWait(80);

    QCOMPARE(rejectSpy.count(), 1);
    QCOMPARE(receivedSpy.count(), 0);
    QCOMPARE(sync.pendingFileCount(), 0);
    QVERIFY(port.files().isEmpty());

    const QString reason = rejectSpy.at(0).at(0).toString();
    QVERIFY2(reason.contains(QStringLiteral("剪贴板")), qPrintable(reason));
}

// 没有清单的落盘（「发送文件…」/ 拖入）不该动剪贴板 —— 用户只是收了个文件，
// 剪贴板里凭空多出东西比「没同步」更让人意外。
void TestClipboardSync::strayFileDoesNotTouchClipboard()
{
    lanrdp::ClipboardSync sync;
    FakeClipboardPort      port;
    sync.setPort(&port);

    sync.noteReceivedFile(QStringLiteral("C:/recv/x.txt"));
    QCOMPARE(port.filesWriteCount, 0);
    QCOMPARE(port.files(), QStringList());
}

// 混选「文件 + 目录」：只把文件挑出来发，别整批不动。
//
// 目录传不了是设计（FILE_* 是逐文件传的，没有「建目录 + 铺子树」）；
// 但整批跳过也不行 —— 用户复选一个目录加两个文件，期望的显然是那两个文件。
// 顺手守住两条：不存在的路径（复制完被删/搬走）与非本地 URL 都不该进清单。
void TestClipboardSync::mixedSelectionSendsOnlyFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();

    auto createFile = [&root](const QString &name) {
        const QString path = QDir(root).absoluteFilePath(name);
        QFile         file(path);
        if (!file.open(QIODevice::WriteOnly))
            return QString();
        file.write(QByteArrayLiteral("payload"));
        return path;
    };

    const QString first  = createFile(QStringLiteral("a.txt"));
    const QString second = createFile(QStringLiteral("b.txt"));
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(QDir(root).mkpath(QStringLiteral("sub")));

    const QList<QUrl> urls{
        QUrl::fromLocalFile(first),
        QUrl::fromLocalFile(QDir(root).absoluteFilePath(QStringLiteral("sub"))),      // 目录
        QUrl::fromLocalFile(QDir(root).absoluteFilePath(QStringLiteral("gone.txt"))), // 不存在
        QUrl(QStringLiteral("https://example.com/x.png")),                            // 非本地
        QUrl::fromLocalFile(second),
    };

    // 顺序也要保住：先复制的先发，接收侧按清单凑批
    QCOMPARE(lanrdp::localFilesFromUrls(urls), (QStringList{first, second}));

    // 全都不合格时返回空 —— 上层据此不清空「待发」、也不点亮按钮
    QVERIFY(lanrdp::localFilesFromUrls(
                {QUrl::fromLocalFile(QDir(root).absoluteFilePath(QStringLiteral("sub")))})
                .isEmpty());
    QVERIFY(lanrdp::localFilesFromUrls({}).isEmpty());
}

QTEST_GUILESS_MAIN(TestClipboardSync)

#include "test_clipboardsync.moc"
