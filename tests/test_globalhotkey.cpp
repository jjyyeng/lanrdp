#include <QtTest/QtTest>

#include <QSet>
#include <QSignalSpy>
#include <QStringList>

#include "app/GlobalHotkey.h"

// 全局热键（遮挡 + 急停）的地基。
//
// 这一层能测的只有「解析」与「注册策略」两件事：真实的 RegisterHotKey 在
// 无桌面会话（以及 CI）里必然失败，所以注册行为用假平台注入 —— 否则
// 「组合被别的程序占用时静默降级」这条最容易退化的分支根本跑不到。
// 真的按下热键要真机验：`RegisterHotKey` 在 offscreen 会话里必然失败。

namespace {

QString describe(const lanrdp::HotkeyCombination &combination)
{
    return QStringLiteral("%1|%2").arg(int(combination.modifiers)).arg(combination.key);
}

// 可编程的假平台层：`occupied` 模拟「组合被别的程序占用了」。
class FakePlatform : public lanrdp::HotkeyPlatform
{
public:
    QSet<QString> occupied;   // 别的程序已占用
    QSet<QString> live;       // 本进程已注册（"id@组合"）
    QStringList   log;        // 调用顺序，用来断言「换组合时先撤旧的」

    bool registerHotkey(int id, const lanrdp::HotkeyCombination &combination) override
    {
        const QString key = describe(combination);
        if (occupied.contains(key))
            return false;
        live.insert(QStringLiteral("%1@%2").arg(id).arg(key));
        log.append(QStringLiteral("reg:%1:%2").arg(id).arg(key));
        return true;
    }

    void unregisterHotkey(int id) override
    {
        log.append(QStringLiteral("unreg:%1").arg(id));
        const QString prefix = QStringLiteral("%1@").arg(id);
        for (auto it = live.begin(); it != live.end();) {
            if (it->startsWith(prefix))
                it = live.erase(it);
            else
                ++it;
        }
    }
};

} // namespace

class TestGlobalHotkey : public QObject
{
    Q_OBJECT

private slots:
    void parsesSupportedCombinations();
    void rejectsUnsafeOrUnsupportedSequences();
    void occupiedCombinationFailsWithoutBlockingTheOthers();
    void activatesOnlyBoundIds();
    void duplicateCombinationIsRefused();
    void rebindingSameIdReleasesTheOldCombination();
};

void TestGlobalHotkey::parsesSupportedCombinations()
{
    lanrdp::HotkeyCombination combination;

    QVERIFY(lanrdp::parseHotkeySequence(QStringLiteral("Ctrl+Alt+H"), &combination));
    QCOMPARE(combination.key, int(Qt::Key_H));
    QVERIFY2(combination.modifiers.testFlag(Qt::ControlModifier), "Ctrl 丢了");
    QVERIFY2(combination.modifiers.testFlag(Qt::AltModifier), "Alt 丢了");
    QVERIFY2(!combination.modifiers.testFlag(Qt::ShiftModifier), "不该凭空多出 Shift");

    QVERIFY(lanrdp::parseHotkeySequence(QStringLiteral("Ctrl+Alt+F12"), &combination));
    QCOMPARE(combination.key, int(Qt::Key_F12));

    // 前后空白要容忍：菜单 / 配置里抄进来常带空格
    QVERIFY(lanrdp::parseHotkeySequence(QStringLiteral("  Ctrl+Alt+0  "), &combination));
    QCOMPARE(combination.key, int(Qt::Key_0));
}

void TestGlobalHotkey::rejectsUnsafeOrUnsupportedSequences()
{
    lanrdp::HotkeyCombination combination;

    // 空串
    QVERIFY(!lanrdp::parseHotkeySequence(QString(), &combination));
    // 没有修饰键：会把一个普通按键从整台机器上抢走
    QVERIFY2(!lanrdp::parseHotkeySequence(QStringLiteral("H"), &combination),
             "裸键不能当全局热键");
    // 只有 Shift：同样抢得太狠，而且 Shift+H 是日常打字
    QVERIFY(!lanrdp::parseHotkeySequence(QStringLiteral("Shift+H"), &combination));
    // 多段序列不是「按一下」
    QVERIFY(!lanrdp::parseHotkeySequence(QStringLiteral("Ctrl+Alt+K, Ctrl+C"), &combination));
    // 白名单之外的键：解析不出来就注册不进去，必须提前拒绝
    QVERIFY(!lanrdp::parseHotkeySequence(QStringLiteral("Ctrl+Enter"), &combination));
    QVERIFY(!lanrdp::parseHotkeySequence(QStringLiteral("Ctrl+Alt+Left"), &combination));
    // out 为空指针
    QVERIFY(!lanrdp::parseHotkeySequence(QStringLiteral("Ctrl+Alt+H"), nullptr));
}

void TestGlobalHotkey::occupiedCombinationFailsWithoutBlockingTheOthers()
{
    lanrdp::HotkeyCombination taken;
    QVERIFY(lanrdp::parseHotkeySequence(QStringLiteral("Ctrl+Alt+H"), &taken));

    // ⚠️ 声明顺序有意：`platform` 必须先构造（= 后析构）。
    //    `~GlobalHotkey` 会回头调 `platform_->unregisterHotkey()` 撤热键，
    //    反过来写就是踩一个已经死掉的对象 —— 症状是退出码 139 而没有任何断言失败。
    FakePlatform         platform;
    lanrdp::GlobalHotkey hotkeys;
    platform.occupied.insert(describe(taken));
    hotkeys.setPlatform(&platform);

    QSignalSpy failed(&hotkeys, &lanrdp::GlobalHotkey::bindFailed);

    QVERIFY2(!hotkeys.bind(1, QStringLiteral("Ctrl+Alt+H")), "被占用时必须如实失败");
    QCOMPARE(failed.count(), 1);
    QVERIFY2(failed.takeFirst().at(2).toString().contains(QStringLiteral("占用")),
             "失败原因要能让人接手排查");

    // 关键：一条抢不到不能拖累另一条 —— 热键不是功能主体，菜单项才是。
    QVERIFY2(hotkeys.bind(2, QStringLiteral("Ctrl+Alt+F12")), "另一条热键必须照常注册成功");
}

void TestGlobalHotkey::activatesOnlyBoundIds()
{
    // ⚠️ 声明顺序有意：`platform` 必须先构造（= 后析构）。
    //    `~GlobalHotkey` 会回头调 `platform_->unregisterHotkey()` 撤热键，
    //    反过来写就是踩一个已经死掉的对象 —— 症状是退出码 139 而没有任何断言失败。
    FakePlatform         platform;
    lanrdp::GlobalHotkey hotkeys;
    hotkeys.setPlatform(&platform);
    QVERIFY(hotkeys.bind(1, QStringLiteral("Ctrl+Alt+H")));

    QSignalSpy fired(&hotkeys, &lanrdp::GlobalHotkey::activated);

    hotkeys.activate(1);
    QCOMPARE(fired.count(), 1);
    QCOMPARE(fired.takeFirst().at(0).toInt(), 1);

    // WM_HOTKEY 是线程可见的：别的组件投来的 id 不能当成自己的热键。
    hotkeys.activate(99);
    QCOMPARE(fired.count(), 0);
}

void TestGlobalHotkey::duplicateCombinationIsRefused()
{
    // ⚠️ 声明顺序有意：`platform` 必须先构造（= 后析构）。
    //    `~GlobalHotkey` 会回头调 `platform_->unregisterHotkey()` 撤热键，
    //    反过来写就是踩一个已经死掉的对象 —— 症状是退出码 139 而没有任何断言失败。
    FakePlatform         platform;
    lanrdp::GlobalHotkey hotkeys;
    hotkeys.setPlatform(&platform);

    QVERIFY(hotkeys.bind(1, QStringLiteral("Ctrl+Alt+H")));

    QSignalSpy failed(&hotkeys, &lanrdp::GlobalHotkey::bindFailed);
    QVERIFY2(!hotkeys.bind(2, QStringLiteral("Ctrl+Alt+H")), "同一组合不该能绑两次");
    QCOMPARE(failed.count(), 1);
    QVERIFY2(failed.takeFirst().at(2).toString().contains(QStringLiteral("重复")),
             "重复的组合要说出是与哪一条撞了");
}

void TestGlobalHotkey::rebindingSameIdReleasesTheOldCombination()
{
    // ⚠️ 声明顺序有意：`platform` 必须先构造（= 后析构）。
    //    `~GlobalHotkey` 会回头调 `platform_->unregisterHotkey()` 撤热键，
    //    反过来写就是踩一个已经死掉的对象 —— 症状是退出码 139 而没有任何断言失败。
    FakePlatform         platform;
    lanrdp::GlobalHotkey hotkeys;
    hotkeys.setPlatform(&platform);

    QVERIFY(hotkeys.bind(1, QStringLiteral("Ctrl+Alt+H")));
    QVERIFY(hotkeys.bind(1, QStringLiteral("Ctrl+Alt+F12")));

    // 旧组合必须先撤掉：留着就是「同一个动作挂了两条热键」，
    // 而后台那一条再也点不掉（界面上只有一个开关）。
    QVERIFY2(platform.log.contains(QStringLiteral("unreg:1")), "换组合前没有注销旧的");
    QCOMPARE(platform.live.size(), 1);
}

QTEST_MAIN(TestGlobalHotkey)
#include "test_globalhotkey.moc"
