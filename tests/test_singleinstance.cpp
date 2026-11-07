// SingleInstanceGuard 的单测。
//
// 判定语义与 fail-open 是这里唯一要守住的东西：
//   - listen 成功后，notifyRunning 能找到它并把 raiseRequested 送回；
//   - 没人听时 notifyRunning 必须返回 false（新实例照常启动的依据）；
//   - 监听释放（析构 / stop）后名字必须立刻可复用 —— 否则进程退了还拦人。
// key 全部带 pid，避免和机器上真实运行的 lanrdp 实例相撞。

#include "app/SingleInstance.h"

#include <QSignalSpy>
#include <QTest>

using lanrdp::SingleInstanceGuard;

namespace {

QString uniqueKey(const char *tag)
{
    return QStringLiteral("lanrdp-test-%1-%2").arg(tag).arg(QCoreApplication::applicationPid());
}

} // namespace

class TestSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void listenSucceeds();
    void notifyRunningFindsTheListener();
    void raiseRequestArrives();
    void nobodyListeningMeansNotifyFails();
    void nameIsReusableAfterDestroy();
    void stopReleasesTheName();
    void repeatedListenLeavesOnePipeline();
};

void TestSingleInstance::listenSucceeds()
{
    SingleInstanceGuard guard;
    QVERIFY(guard.listen(uniqueKey("listen")));
    QCOMPARE(guard.key(), uniqueKey("listen"));
}

void TestSingleInstance::notifyRunningFindsTheListener()
{
    const QString key = uniqueKey("notify");

    SingleInstanceGuard guard;
    QVERIFY(guard.listen(key));
    QVERIFY(SingleInstanceGuard::notifyRunning(key));
}

void TestSingleInstance::raiseRequestArrives()
{
    const QString key = uniqueKey("raise");

    SingleInstanceGuard guard;
    QVERIFY(guard.listen(key));

    QSignalSpy spy(&guard, &SingleInstanceGuard::raiseRequested);
    QVERIFY(SingleInstanceGuard::notifyRunning(key));
    // 信号多半在 notifyRunning 的同步等待（内部泵事件）期间就已发出，
    // 也可能晚一点才走事件循环 —— 两种时机都要认。
    spy.wait(2000);
    QVERIFY(spy.count() >= 1);
}

void TestSingleInstance::nobodyListeningMeansNotifyFails()
{
    QVERIFY(!SingleInstanceGuard::notifyRunning(uniqueKey("nobody")));
}

void TestSingleInstance::nameIsReusableAfterDestroy()
{
    const QString key = uniqueKey("reuse");

    {
        SingleInstanceGuard first;
        QVERIFY(first.listen(key));
    }

    SingleInstanceGuard second;
    QVERIFY(second.listen(key));
    QVERIFY(SingleInstanceGuard::notifyRunning(key));
}

void TestSingleInstance::stopReleasesTheName()
{
    const QString key = uniqueKey("stop");

    SingleInstanceGuard guard;
    QVERIFY(guard.listen(key));
    guard.stop();
    QVERIFY(!SingleInstanceGuard::notifyRunning(key));

    // 交出名字后能重新监听（设置里反复勾选/取消的场景）。
    QVERIFY(guard.listen(key));
    QVERIFY(SingleInstanceGuard::notifyRunning(key));
}

void TestSingleInstance::repeatedListenLeavesOnePipeline()
{
    const QString key = uniqueKey("repeat");

    SingleInstanceGuard guard;
    QVERIFY(guard.listen(key));
    // 重复开启（设置里勾一次、再勾一次；或者将来别处再调一次）不能留下第二条
    // 管道：Windows 允许同名多实例，旧的那条不会被 stop() 关掉，名字就永远放不出去。
    QVERIFY(guard.listen(key));
    QVERIFY(SingleInstanceGuard::notifyRunning(key));

    guard.stop();
    QVERIFY2(!SingleInstanceGuard::notifyRunning(key),
             "stop() 之后名字必须真的放掉 —— 这里漏了说明还有一条管道在听");
}

QTEST_MAIN(TestSingleInstance)
#include "test_singleinstance.moc"
