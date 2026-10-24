#include <QtTest/QtTest>

#include "host/TokenGate.h"

#include <cstdint>

#include <QVector>

// 假时钟：手动推进，从而不必真的 sleep 就能验证 5 分钟空闲超时。
// 起点取一个非 0 的时刻，避免与「尚未记录过活动」混淆。
struct FakeClock
{
    std::int64_t now = 1'000'000;

    void advance(std::int64_t ms) { now += ms; }

    // 按值捕获 this 指针，因此外部 advance() 能影响到 TokenGate 里那份时钟。
    lanrdp::TokenGate::Clock fn() { return [this] { return now; }; }
};

// 输入令牌状态机（纯逻辑，身份由 issue() 发放的单调 id，零 IO）
// 扩展为三态（空闲 / 被持有 / 已锁定）+ 空闲自动让出（时钟注入，不 sleep）
class TestTokenGate : public QObject
{
    Q_OBJECT

private slots:
    // ---- 身份发放 ----
    void issueNeverRepeats();
    void distinctTokensAreIndependent();
    void invalidTokenIsRejected();

    // ---- 原有的非抢占语义 ----
    void requestGrantsWhenNoHolder();
    void requestDeniedWhenAnotherHolds();
    void holderRequestIsIdempotent();
    void releaseOnlyAffectsHolder();
    void removeFreesToken();
    void allowInputOnlyForHolder();

    // ---- 锁定 ----
    void lockRejectsAllRequests();
    void unlockRestoresGranting();
    void lockDoesNotAutoReleaseHolder();
    void removeKeepsLock();

    // ---- 空闲自动让出 ----
    void idleHolderLosesTokenAfterTimeout();
    void requestLazilyExpiresIdleHolder();
    // 没有发生惰性过期时必须报 0 —— 否则调用方会给「根本没被让出」的人补发
    // TokenRelease，把他手里可能正握着的令牌误释放掉
    void requestReportsNoExpiryWhenNone();
    void touchExtendsIdleDeadline();
    void nonHolderTouchIsIgnored();
    void expireWithoutHolderDoesNothing();
};

// 身份绝不复用 —— 这正是「会话析构后地址被新会话复用」不会继承令牌的结构性保证：
// 指针地址会复用，issue() 发过的值不会。
void TestTokenGate::issueNeverRepeats()
{
    lanrdp::TokenGate gate;

    QVector<lanrdp::TokenGate::Token> seen;
    for (int i = 0; i < 1000; ++i) {
        const auto t = gate.issue();
        QVERIFY2(t != 0, "issue() 不该发放无效身份（0 保留给「无」）");
        QVERIFY2(!seen.contains(t), "issue() 发过的身份不允许再次出现");
        seen.append(t);
    }
}

// 两个身份互相独立：一方持有 / 释放，绝不影响另一方（旧实现里
// 「同一地址的新会话」会被当成原持有者，这里从身份层就排除了）。
void TestTokenGate::distinctTokensAreIndependent()
{
    lanrdp::TokenGate gate;
    const auto        older = gate.issue();   // 模拟「上一任会话」
    const auto        newer = gate.issue();   // 模拟「复用了同一地址的新会话」

    QVERIFY(gate.request(older));
    gate.remove(older);                       // 上一任断开

    QVERIFY2(!gate.holds(older), "断开的一任不该还持有令牌");
    QVERIFY2(!gate.holds(newer), "新一任不能凭旧的持有关系白拿令牌");
    QVERIFY(gate.holder() == 0);

    QVERIFY(gate.request(newer));             // 新一任要自己申请
    QVERIFY(gate.holds(newer));
    QVERIFY(!gate.holds(older));
}

void TestTokenGate::invalidTokenIsRejected()
{
    lanrdp::TokenGate gate;

    QVERIFY(!gate.request(0));      // 0 = 无效身份（从前的 nullptr）
    QVERIFY(gate.holder() == 0);
    QVERIFY(!gate.holds(0));
    QVERIFY(!gate.allowInput(0));

    gate.touch(0);                  // 空 id 同样安全（不该崩）
    QVERIFY(gate.expireIfIdle() == 0);
}

void TestTokenGate::requestGrantsWhenNoHolder()
{
    lanrdp::TokenGate gate;
    QVERIFY(gate.holder() == 0);

    const auto a = gate.issue();

    QVERIFY(gate.request(a));
    QCOMPARE(gate.holder(), a);
    QVERIFY(gate.holds(a));
}

void TestTokenGate::requestDeniedWhenAnotherHolds()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));

    QVERIFY(!gate.request(b));    // A 持有，B 请求被拒（非抢占）
    QCOMPARE(gate.holder(), a);   // 令牌仍在 A
    QVERIFY(!gate.holds(b));
}

void TestTokenGate::holderRequestIsIdempotent()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();

    QVERIFY(gate.request(a));
    QVERIFY(gate.request(a));     // 持有者再次请求仍成功
    QCOMPARE(gate.holder(), a);
}

void TestTokenGate::releaseOnlyAffectsHolder()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));

    gate.release(b);              // 非持有者 release 无效
    QCOMPARE(gate.holder(), a);

    gate.release(a);              // 持有者 release 生效
    QVERIFY(gate.holder() == 0);
    QVERIFY(!gate.holds(a));
}

void TestTokenGate::removeFreesToken()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));

    gate.remove(a);               // A 断开 -> 令牌空出
    QVERIFY(gate.holder() == 0);

    QVERIFY(gate.request(b));     // B 现在能拿到
    QCOMPARE(gate.holder(), b);
}

void TestTokenGate::allowInputOnlyForHolder()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));

    QVERIFY(gate.allowInput(a));
    QVERIFY(!gate.allowInput(b));
}

// 锁定后，任何人都拿不到令牌（这是「收回输入权」能生效的前提：
// 否则对方下一次点击画面就会重新抢回）。
void TestTokenGate::lockRejectsAllRequests()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    gate.lock();
    QVERIFY(gate.isLocked());

    QVERIFY(!gate.request(a));
    QVERIFY(!gate.request(b));
    QVERIFY(gate.holder() == 0);
    QVERIFY(!gate.allowInput(a));
}

void TestTokenGate::unlockRestoresGranting()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();

    gate.lock();
    gate.unlock();
    QVERIFY(!gate.isLocked());

    QVERIFY(gate.request(a));
    QCOMPARE(gate.holder(), a);
}

// lock() 只负责「不再授予」，要不要顺便踢掉现有持有者由调用方决定
// （收回输入权的三段式是 lock -> release -> 通知，顺序由 UI 层保证）。
void TestTokenGate::lockDoesNotAutoReleaseHolder()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));
    gate.lock();

    QCOMPARE(gate.holder(), a);   // 没有自动释放
    QVERIFY(gate.holds(a));

    gate.release(a);              // 调用方显式释放
    QVERIFY(gate.holder() == 0);
    QVERIFY(!gate.request(b));    // 释放之后依然被锁着
}

// 锁定是「粘性」的：会话断开不会解除锁定，只能由机主显式解锁。
void TestTokenGate::removeKeepsLock()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));
    gate.lock();
    gate.remove(a);               // 持有者断开

    QVERIFY(gate.holder() == 0);
    QVERIFY(gate.isLocked());     // 锁还在
    QVERIFY(!gate.request(b));

    gate.unlock();
    QVERIFY(gate.request(b));     // 解锁后恢复
}

void TestTokenGate::idleHolderLosesTokenAfterTimeout()
{
    FakeClock         clk;
    lanrdp::TokenGate gate(clk.fn());
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));  // t0 拿到令牌

    clk.advance(lanrdp::TokenGate::kIdleTimeoutMs - 1);
    QVERIFY2(gate.expireIfIdle() == 0, "差 1 ms 不该释放");

    clk.advance(1);
    QCOMPARE(gate.expireIfIdle(), a);  // 到点释放，并返回是谁被释放
    QVERIFY(gate.holder() == 0);

    QVERIFY(gate.request(b));  // 令牌空出，其他控制端可以拿到
}

// 惰性判定：没人来请求时靠定时器兜底，但一旦有人请求也应立即发现持有者已空闲。
void TestTokenGate::requestLazilyExpiresIdleHolder()
{
    FakeClock         clk;
    lanrdp::TokenGate gate(clk.fn());
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));
    clk.advance(lanrdp::TokenGate::kIdleTimeoutMs);

    lanrdp::TokenGate::Token expired = 0;
    QVERIFY2(gate.request(b, &expired), "空闲超时的持有者不该继续占着令牌");
    QCOMPARE(gate.holder(), b);
    // 让出的是谁必须报给调用方：他靠这个补发 TokenRelease。少这一步，
    // 被过期的那位界面仍显示握着操作权，之后每次输入都被静默拒绝。
    QCOMPARE(expired, a);
}

// 没有发生惰性过期时必须报 0。报错了的后果不是「少发一帧」而是「误发一帧」：
// 被误报的那位会收到 TokenRelease 并按「已让出」处理。
void TestTokenGate::requestReportsNoExpiryWhenNone()
{
    lanrdp::TokenGate gate;
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));

    // a 还在空闲期内，b 抢不到：这一路不该报「有人被过期」
    lanrdp::TokenGate::Token expired = 999;    // 脏初值，用来验证函数会重置
    QVERIFY2(!gate.request(b, &expired), "a 仍在空闲期内，b 不该拿到");
    QCOMPARE(expired, lanrdp::TokenGate::Token(0));
    QCOMPARE(gate.holder(), a);

    // 首次授予同样不该报过期
    lanrdp::TokenGate fresh;
    const auto       c = fresh.issue();
    expired            = 999;
    QVERIFY(fresh.request(c, &expired));
    QCOMPARE(expired, lanrdp::TokenGate::Token(0));

    // 不传出参（旧写法）必须照常可用
    lanrdp::TokenGate plain;
    const auto        d = plain.issue();
    QVERIFY(plain.request(d));
}

void TestTokenGate::touchExtendsIdleDeadline()
{
    FakeClock         clk;
    lanrdp::TokenGate gate(clk.fn());
    const auto        a = gate.issue();

    QVERIFY(gate.request(a));

    clk.advance(4 * 60 * 1000);  // 4 分钟：持有者动了一下（点击 / 按键 / 滚轮）
    gate.touch(a);

    clk.advance(4 * 60 * 1000);  // 距上次活动只有 4 分钟
    QVERIFY2(gate.expireIfIdle() == 0, "touch 应当把超时重新计时");
    QVERIFY(gate.holds(a));

    clk.advance(60 * 1000);      // 距上次活动满 5 分钟
    QCOMPARE(gate.expireIfIdle(), a);
}

// 只有持有者本人的活动才算数：别的控制端的输入是噪音。
void TestTokenGate::nonHolderTouchIsIgnored()
{
    FakeClock         clk;
    lanrdp::TokenGate gate(clk.fn());
    const auto        a = gate.issue();
    const auto        b = gate.issue();

    QVERIFY(gate.request(a));
    clk.advance(6 * 60 * 1000);

    gate.touch(b);  // 非持有者 touch，不该续期
    QCOMPARE(gate.expireIfIdle(), a);

    gate.touch(0);  // 空 id 同样安全
    QVERIFY(gate.holder() == 0);
}

void TestTokenGate::expireWithoutHolderDoesNothing()
{
    FakeClock         clk;
    lanrdp::TokenGate gate(clk.fn());

    QVERIFY(gate.expireIfIdle() == 0);  // 从没被持有过

    gate.lock();
    QVERIFY(gate.expireIfIdle() == 0);  // 锁定时无持有者，不该误报
    QVERIFY(gate.isLocked());
}

QTEST_GUILESS_MAIN(TestTokenGate)
#include "test_tokengate.moc"
