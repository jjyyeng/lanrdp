#include "net/AuthBackoff.h"

namespace lanrdp {

AuthBackoff::AuthBackoff(Config config)
    : config_(config)
{
}

// 惰性复位：静默超过 resetWindowMs 就当作「这个来源已经放下了」，计数归零。
// 之所以不另起定时器 —— 一条 TCP 服务端上定时器越少越好，而访问该桶时顺手判一下
// 的语义完全等价（下一次尝试时才需要正确的计数）。
//
// ⚠️ 有意**只清零、不 erase**（评审建议过删桶）：本类不做全表扫描，删单个桶只能删掉
// 「恰好又被访问到」的那个，删不到其余 —— 而条目数 = 历史不同源 IP 数，局域网内最多
// 几百条 × 几十字节。真要回收得另加定期 sweep，收益不抵复杂度，故不做。
void AuthBackoff::expireIfQuiet(const QString &sourceIp, qint64 nowMs)
{
    auto it = entries_.find(sourceIp);
    if (it == entries_.end())
        return;
    if (nowMs - it->lastFailureAt > config_.resetWindowMs) {
        it->consecutiveFailures = 0;
        it->nextAllowedAt       = 0;
    }
}

const AuthBackoff::Entry *AuthBackoff::findLive(const QString &sourceIp, qint64 nowMs) const
{
    auto it = entries_.constFind(sourceIp);
    if (it == entries_.constEnd())
        return nullptr;
    // 静默超窗的桶对查询等价于「不存在」—— 查询路径不改状态（复位留给下一次记账）。
    if (nowMs - it->lastFailureAt > config_.resetWindowMs)
        return nullptr;
    return &it.value();
}

bool AuthBackoff::isAllowed(const QString &sourceIp, qint64 nowMs) const
{
    const Entry *e = findLive(sourceIp, nowMs);
    if (!e)
        return true;
    return nowMs >= e->nextAllowedAt;
}

qint64 AuthBackoff::remainingDelayMs(const QString &sourceIp, qint64 nowMs) const
{
    const Entry *e = findLive(sourceIp, nowMs);
    if (!e)
        return 0;
    const qint64 remaining = e->nextAllowedAt - nowMs;
    return remaining > 0 ? remaining : 0;
}

qint64 AuthBackoff::noteFailure(const QString &sourceIp, qint64 nowMs)
{
    expireIfQuiet(sourceIp, nowMs);

    Entry &e = entries_[sourceIp];
    ++e.consecutiveFailures;

    // delay = min(base × 2^(n-1), cap)
    //
    // 指数部分用「左移 + 提前封顶」而不是 std::pow：位移是整数运算、不会因
    // 浮点误差在边界上差 1 ms；而 n 可能被攻击者推得很大（每次失败 n+1），
    // 所以必须尽早夹住，不能在算完 2^n 之后再比大小。
    qint64 delay = config_.baseDelayMs;
    for (int i = 1; i < e.consecutiveFailures; ++i) {
        if (delay >= config_.maxDelayMs) {
            delay = config_.maxDelayMs;
            break;
        }
        delay *= 2;
    }
    if (delay > config_.maxDelayMs)
        delay = config_.maxDelayMs;

    e.lastFailureAt = nowMs;
    e.nextAllowedAt = nowMs + delay;
    return delay;
}

void AuthBackoff::noteSuccess(const QString &sourceIp)
{
    entries_.remove(sourceIp);
}

int AuthBackoff::failureCount(const QString &sourceIp, qint64 nowMs) const
{
    // 与 isAllowed 用同一套「活条目」判据：静默超窗的桶对外等价于不存在，
    // 否则日志/UI 会出现「显示还有 3 次失败、但已经放行」的自相矛盾。
    const Entry *e = findLive(sourceIp, nowMs);
    return e ? e->consecutiveFailures : 0;
}

void AuthBackoff::clear()
{
    entries_.clear();
}

} // namespace lanrdp
