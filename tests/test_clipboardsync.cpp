#include <QtTest/QtTest>

#include <QSignalSpy>
#include <QString>
#include <QTimer>
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
        emit changed();
    }

    int  writeCount     = 0;
    bool asyncDelivery  = false;  // true 时 setText 的 changed 延到事件循环下一拍

private:
    QString text_;
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

QTEST_GUILESS_MAIN(TestClipboardSync)
#include "test_clipboardsync.moc"
