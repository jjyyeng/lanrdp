#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

#include "net/Session.h"  // moc 需要完整类型（信号携带 Session*）

namespace lanrdp {

namespace auth {

// nonce 长度（字节）。挑战-响应的挑战值固定这么长。
constexpr int kNonceSize = 32;

// 口令 -> sha256 原始摘要（固定 32 字节）。
QByteArray hashPassword(const QString &password);

// 生成一个 32 字节 nonce。来源是操作系统熵源（QRandomGenerator::system()），
// 不是 global() 的伪随机流 —— 后者在多进程同种子时会撞。
// 每个连接、每一轮都必须重新生成，否则重放防护形同虚设。
QByteArray makeNonce();

// 计算挑战响应：HMAC-SHA256(key = 口令的 UTF-8 字节, message = nonce)。
// 注意 key 用的是**口令原文**，不是 sha256 摘要。
QByteArray computeResponse(const QString &password, const QByteArray &nonce);

// 校验响应（定长摘要逐字节异或的常量时间实现，与 verifyRequest 同构）。
bool verifyResponse(const QByteArray &response, const QString &expectedPassword,
                    const QByteArray &nonce);

// 校验 HANDSHAKE_REQ 的载荷是否匹配期望口令（定长摘要比对的常量时间实现）。
// 保留用于兼容/测试；现行协议已改为挑战-响应，线上不再传摘要。
bool verifyRequest(const QByteArray &requestPayload, const QString &expectedPassword);

} // namespace auth

// 服务端侧握手（挑战-响应）：挂在一条 Session 上，**主动发** HANDSHAKE_CHALLENGE
// （32 B nonce），收到 HANDSHAKE_RESPONSE 后比对，回 HANDSHAKE_OK / HANDSHAKE_DENY。
//
// 为什么是服务端先发：客户端「先发」时，它必须先拿出口令去算点东西，
// 那个东西就是可重放的通行证（旧协议发 sha256 摘要，嗅探者原样重发即可登录）。
// 改成服务端先给一次性 nonce 后，线上任何与口令相关的字节**都只对这一次连接有效**。
class AuthServer : public QObject
{
    Q_OBJECT

public:
    explicit AuthServer(QString password, QObject *parent = nullptr);

    void attach(Session *session);
    Session *session() const { return session_; }

    // 接受条件：鉴权通过（口令正确）后、回 HANDSHAKE_OK 之前再问一次。
    // 返回 false 时按拒绝处理（回 DENY + emit rejected），用于满员等容量控制。
    // 未设置时一律接受。
    using AcceptGuard = std::function<bool()>;
    void setAcceptGuard(AcceptGuard guard) { acceptGuard_ = std::move(guard); }

signals:
    void authenticated(lanrdp::Session *session);
    void rejected(lanrdp::Session *session);

private:
    // 生成新 nonce 并发出 CHALLENGE。只在 nonce_ 为空时被调用 ——
    // 每轮握手只发一次，中途换 nonce 会让客户端已算好的响应当场作废。
    void sendChallenge();

    QString      password_;
    Session     *session_     = nullptr;
    QByteArray   nonce_;
    AcceptGuard  acceptGuard_;

    // 一次性闩：给出 OK / DENY 结论后置位，之后的任何响应一律忽略。
    // 对称于 AuthClient 的 done_。缺了它，同一条连接上重发响应会让
    // authenticated 触发多次 —— 上层按「每次 authenticated 就 append + 接一份收帧 lambda」
    // 实现时，同一会话会被注入两次输入、还独占 4 个名额里的 4 个。
    bool         done_ = false;
};

// 客户端侧握手（挑战-响应）：发一帧空 HANDSHAKE_REQ 声明 + 发起，
// 等服务端 CHALLENGE，回 HMAC-SHA256(口令, nonce)，再等 OK / DENY。
class AuthClient : public QObject
{
    Q_OBJECT

public:
    AuthClient(Session *session, QString password, QObject *parent = nullptr);

    void start();

signals:
    void succeeded();
    void failed();

private:
    Session *session_ = nullptr;
    QString  password_;
    bool     done_ = false;
    // 已回应过的 nonce（防服务端重复发 CHALLENGE 时回两份响应）。
    QByteArray answeredNonce_;
};

} // namespace lanrdp
