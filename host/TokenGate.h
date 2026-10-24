#pragma once

#include <cstdint>
#include <functional>

namespace lanrdp {

// 输入令牌：保证同时只有一个会话能输入。
//
// 身份是 TokenGate 自己发放的**单调 id**（`issue()`），**不是**调用方的指针。
// 为什么不用裸指针作身份：调用方拿 `Session*` 当 key 时，一个 Session 析构后
// 新会话若正好落在同一地址，就会**继承**上一个持有者的令牌 —— `holds(news)` 会
// 返回 true、`request(news)` 直接幂等成功。指针地址会被复用，id 不会。
//
// 三态：
// 空闲 —— 无持有者、未锁定：任何有效 id 的 request 都能立即拿到；
// 被持有 —— 有持有者：仅持有者本人 request 幂等，其余拒绝（非抢占、无排队）；
// 已锁定 —— 锁定期间 request **一律拒绝**，直到 unlock()。
//
// 为什么要「锁定」这一态：如果收回输入权只做 release()，控制端下一次点击画面
// 就会重新 request，而此刻恰好无人持有 → 立刻又被授予，收回动作形同虚设。
//
// 空闲自动让出：持有者连续 kIdleTimeoutMs 没有「真实活动」时，令牌自动空出。
// 「真实活动」只算持有者本人的点击 / 滚轮 / 按键 —— 纯鼠标移动不算
// （控制端的 MouseMove 是持续上报的，算进去会导致超时永远触发不了）。
//
// 规则：
// - 无持有者且未锁定时 request 立即授予；
// - 有持有者且非本人时 request 被拒（静默，调用方决定是否稍后重试）；
// - 持有者本人 request 幂等；
// - release 仅持有者本人有效；remove 用于会话断开时强制让出令牌；
// - remove / release **不影响**锁定状态（锁定是机主的意图，粘性保持）。
//
// ⚠️ 线程约定：本类**没有任何内部同步**，只允许从单一线程调用。
//
// 唯一的调用方是 `HostController`，而它跑在**宿主工作线程**上
//（socket 读写 / 输入注入 / 抓屏编码全在那条线程，不在 GUI 线程）。「同一时刻只有
// 一处碰它」仍由外部结构保证 —— 只是那条线程换了名字。⚠️ 因此 **GUI 侧一律不许碰本类**：
// 界面要用的锁定态 / 持有者，只能来自 `HostController` 推过去的 `UiState` 快照。
class TokenGate
{
public:
    // 返回「当前时间（毫秒）」。默认用 std::chrono::steady_clock（单调时钟，
    // 不受系统对时影响）；单测注入假时钟，从而无需 sleep 就能验证超时。
    using Clock = std::function<std::int64_t()>;

    // 会话身份。0 = 无效（nullptr 的替代品）。由 issue() 发放，单调递增、绝不复用。
    using Token = std::uint64_t;

    explicit TokenGate(Clock clock = Clock());

    // 持有者空闲多久后自动让出令牌。
    static constexpr std::int64_t kIdleTimeoutMs = 5 * 60 * 1000;

    // 发放一个新身份。永不返回 0，且与**曾经发放过的任何值**都不相同
    // —— 这正是「会话销毁后地址被复用」不会继承令牌的结构性保证。
    Token issue();

    // 请求令牌。返回是否获得（无持有者或已是持有者时为 true）。
    // `expiredOut` 非空时，把「因空闲超时**正好在这一刻**被让出」的原持有者身份
    // 写出去（没有被过期的人则为 0）。调用方必须据此补发一帧 TokenRelease，
    // 否则那位还以为自己握着操作权，之后每次输入都被静默拒绝、界面毫无反馈。
    bool request(Token who, Token *expiredOut = nullptr);

    // 释放令牌（仅持有者本人有效）。
    void release(Token who);

    // who 是否持有令牌。
    bool holds(Token who) const;

    // who 的输入是否被允许（等价于 holds）。
    bool allowInput(Token who) const;

    // 当前持有者的身份；无持有者时返回 0。
    Token holder() const { return holder_; }

    // 会话断开时调用：若 who 持有令牌则让出。**不清除锁定状态。**
    void remove(Token who);

    // ---- 锁定 ----

    // 锁定输入：此后 request 一律返回 false。**不会自动释放现有持有者**
    // （由调用方按「加锁 -> 释放 -> 通知」三段式自己决定顺序）。
    void lock();

    // 解除锁定，恢复正常的「无人持有即授予」。
    void unlock();

    bool isLocked() const { return locked_; }

    // 记录持有者的一次「真实活动」（点击 / 滚轮 / 按键；纯鼠标移动不要调）。
    // 仅持有者本人有效，其余调用被忽略。
    void touch(Token who);

    // 若持有者已空闲超过 kIdleTimeoutMs，则让出令牌并返回**原持有者的身份**；
    // 否则返回 0。
    Token expireIfIdle();

    // 同上，但由调用方指定「当前时间」（单测用，避免依赖注入时钟）。
    // ⚠️ 形参名不得叫 nowMs —— 那会遮蔽同名的私有成员函数 nowMs()（改坏时静默）。
    Token expireIfIdle(std::int64_t now);

private:
    std::int64_t nowMs() const;

    Token        nextToken_      = 1;    // 下一个待发放的身份（0 保留给「无效」）
    Token        holder_         = 0;    // 当前持有者
    bool         locked_         = false;// 机主是否锁定了输入
    std::int64_t lastActivityAt_ = 0;    // 持有者最后一次「真实活动」的时间戳
    Clock        clock_;                 // 可注入时钟（空 = 用 steady_clock）
};

} // namespace lanrdp
