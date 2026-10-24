#pragma once

#include <QHash>
#include <QString>

namespace lanrdp {

// 登录失败退避（按来源 IP 分桶）。
//
// 背景：认证失败若只是断开连接，攻击者可以立刻重连再猜 —— 内网单连接往返约
// 0.2 ms，(口令) 摘要比对又是微秒级，于是每秒可以试数百次，6 位纯数字口令
// 「小时级」就能撞穿。
//
// 设计要点（三条硬约束，改代码时别破坏）：
//  ① 惩罚**按来源 IP** 记（不含端口 —— 否则每次重连都是新桶，等于没分桶）。
//     不用全局计数：一个人猜错不能把所有人都锁在门外。
//  ② 调用方必须**在新连接建立时**就查 —— 挂在「当前连接」上会被断线重连直接绕过。
//  ③ 本类**不做任何等待 / 睡眠**。它只回答「现在能不能受理」，由调用方决定拒绝。
//     用 sleep 阻塞单线程事件循环会冻住所有连接（含正在推的画面流），等于自 DoS。
//
// 时间源由调用方注入（`nowMs`），不读系统时钟 —— 这样判活/清零逻辑可以脱离
// 真实时间单测（与本项目 TokenGate / AdaptiveBitrate / layoutForCount 同一路子）。
class AuthBackoff
{
public:
    struct Config
    {
        // 首次惩罚时长；之后每失败一次翻倍。
        qint64 baseDelayMs = 1000;
        // 惩罚上限（指数增长在此封顶）。
        qint64 maxDelayMs = 30000;
        // 静默多久后认为该来源「放下了」，计数清零（惰性判定，不是定时器）。
        qint64 resetWindowMs = 15 * 60 * 1000;
    };

    AuthBackoff() = default;
    explicit AuthBackoff(Config config);

    // 该来源此刻是否已被允许重试。未到 nextAllowedAt 时返回 false。
    bool isAllowed(const QString &sourceIp, qint64 nowMs) const;

    // 距离可以重试还差多少毫秒（已放行时为 0）。供调用方记日志用。
    qint64 remainingDelayMs(const QString &sourceIp, qint64 nowMs) const;

    // 记一次「认证比对失败」，返回本次惩罚时长。
    qint64 noteFailure(const QString &sourceIp, qint64 nowMs);

    // 记一次成功 —— 该来源计数清零、立即放行。
    void noteSuccess(const QString &sourceIp);

    // 当前连续失败次数（测试与日志用）。
    // 需要 nowMs 是因为要判「静默超窗 ⇒ 已复位」—— 否则日志会出现
    // 「显示还有 3 次失败，但 isAllowed 已经放行」的自相矛盾。
    int failureCount(const QString &sourceIp, qint64 nowMs) const;

    // 清空全部桶（停止监听时调用）。
    void clear();

private:
    struct Entry
    {
        int    consecutiveFailures = 0;
        qint64 nextAllowedAt       = 0;
        qint64 lastFailureAt       = 0;
    };

    // 惰性维护：顺手判「静默超窗 ⇒ 复位」并返回条目（必要时不存在则返回 nullptr）。
    // 之所以做成 const 也能调用的形态：查询路径不该改变可观测状态，
    // 真正的复位放在下一次 noteFailure/noteSuccess 里做。
    const Entry *findLive(const QString &sourceIp, qint64 nowMs) const;

    // 真复位（只在记账路径调用）。静默超过 resetWindowMs 的桶当作不存在。
    void expireIfQuiet(const QString &sourceIp, qint64 nowMs);

    Config             config_;
    mutable QHash<QString, Entry> entries_;
};

} // namespace lanrdp
