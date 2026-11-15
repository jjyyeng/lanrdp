// SingleInstanceGuard 的单测。
//
// 判定语义与 fail-open 是这里唯一要守住的东西：
//   - listen 成功后，notifyRunning 能找到它并把 raiseRequested 送回；
//   - 没人听时 notifyRunning 必须返回 false（新实例照常启动的依据）；
//   - 监听释放（析构 / stop）后名字必须立刻可复用 —— 否则进程退了还拦人。
// key 全部带 pid，避免和机器上真实运行的 lanrdp 实例相撞。

#include "app/SingleInstance.h"

#include "protocol/Frame.h"

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
    // 名字必须带协议版本，否则新旧两份程序会互相吞掉
    void keyCarriesProtocolVersion();
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

// 名字里必须带**协议版本**。
//
// 同机放着新旧两份程序是常态（旧 dist 备份、从别人那拷来的一份）。名字若写死，
// 双击新版会被旧版吞掉 —— 而版本不一致的症状是「能动但点不动」，属于最难自诊
// 的一类故障。所以这条断言不是形式主义：它守住的是「两份不同协议的程序不会
// 把对方当成自己」。
void TestSingleInstance::keyCarriesProtocolVersion()
{
    const QString key = lanrdp::singleInstanceKey();

    QVERIFY2(!key.isEmpty(), "监听名不能为空");
    QVERIFY2(key.contains(QStringLiteral("lanrdp")), qPrintable(key));
    QVERIFY2(key.contains(QString::number(int(lanrdp::kVersion))),
             qPrintable(QStringLiteral("名字里看不出协议版本：%1").arg(key)));

    // 同一个构建里必须稳定：探测与监听各算一次，飘了就等于「自己拦不住自己」。
    QCOMPARE(lanrdp::singleInstanceKey(), key);
}

QTEST_MAIN(TestSingleInstance)
#include "test_singleinstance.moc"
