#include "host/TokenGate.h"

#include <chrono>

namespace lanrdp {

TokenGate::TokenGate(Clock clock)
    : clock_(std::move(clock))
{
}

std::int64_t TokenGate::nowMs() const
{
    if (clock_)
        return clock_();

    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

TokenGate::Token TokenGate::issue()
{
    // 单调递增、绝不复用：即使调用方把旧 id 留着，也不会与新会话撞上。
    return nextToken_++;
}

bool TokenGate::request(Token who, Token *expiredOut)
{
    if (expiredOut != nullptr)
        *expiredOut = 0;
    if (who == 0)
        return false;
    if (locked_)                      // 锁定期间一律拒绝（直到 unlock）
        return false;

    if (holder_ != 0 && holder_ != who) {
        // 惰性判定：持有者可能早已空闲超时。定时器只是兜底，
        // 真正「有人来抢」时应当立刻发现并让出，不必等下一拍定时器。
        // 让出的是谁必须报给调用方 —— 这条路径和定时器那条是同一件事，
        // 都得通知到人，否则被过期者毫不知情。
        const Token expired = expireIfIdle(nowMs());
        if (expired != 0 && expiredOut != nullptr)
            *expiredOut = expired;
        if (holder_ != 0)
            return false;             // 仍被别人占着
    }

    holder_         = who;
    lastActivityAt_ = nowMs();        // 记录起点，作为空闲超时的基准
    return true;
}

void TokenGate::release(Token who)
{
    if (who != 0 && holder_ == who) {
        holder_         = 0;
        lastActivityAt_ = 0;
    }
}

bool TokenGate::holds(Token who) const
{
    return who != 0 && holder_ == who;
}

bool TokenGate::allowInput(Token who) const
{
    return holds(who);
}

void TokenGate::remove(Token who)
{
    if (who != 0 && holder_ == who) {
        holder_         = 0;
        lastActivityAt_ = 0;
    }
    // 注意：不动 locked_ —— 锁定是机主的意图，粘性保持到显式解锁。
}

// ---- 锁定 ----

void TokenGate::lock()
{
    locked_ = true;
    // 刻意不释放 holder_：要不要踢掉现有持有者、以及先锁后踢还是先踢后锁，
    // 由调用方按「lock -> release -> 通知」三段式决定（顺序反了会留竞态窗口）。
}

void TokenGate::unlock()
{
    locked_ = false;
}

void TokenGate::touch(Token who)
{
    if (who == 0 || holder_ != who)
        return;                       // 非持有者的活动不算数
    lastActivityAt_ = nowMs();
}

TokenGate::Token TokenGate::expireIfIdle()
{
    return expireIfIdle(nowMs());
}

TokenGate::Token TokenGate::expireIfIdle(std::int64_t now)
{
    if (holder_ == 0)
        return 0;
    if (now - lastActivityAt_ < kIdleTimeoutMs)
        return 0;

    const Token released = holder_;
    holder_              = 0;
    lastActivityAt_      = 0;
    return released;                  // 返回是谁被释放，调用方据此发 TokenRelease
}

} // namespace lanrdp
