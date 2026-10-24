#include "net/Auth.h"

#include "net/Session.h"
#include "protocol/Frame.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>

#include <utility>

namespace lanrdp {
namespace auth {

QByteArray hashPassword(const QString &password)
{
    return QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256);
}

QByteArray makeNonce()
{
    // system() 走操作系统熵源；global() 是进程内伪随机流 —— 多进程同种子时会撞。
    // kNonceSize 是 4 的整数倍，所以按 quint32 填满正好、无余数。
    static_assert(kNonceSize % 4 == 0, "nonce 长度必须是 4 的倍数才能按 quint32 填充");

    QByteArray nonce(kNonceSize, '\0');
    auto      *words = reinterpret_cast<quint32 *>(nonce.data());
    QRandomGenerator::system()->generate(words, words + kNonceSize / 4);
    return nonce;
}

QByteArray computeResponse(const QString &password, const QByteArray &nonce)
{
    return QMessageAuthenticationCode::hash(nonce, password.toUtf8(),
                                           QCryptographicHash::Sha256);
}

bool verifyResponse(const QByteArray &response, const QString &expectedPassword,
                    const QByteArray &nonce)
{
    const QByteArray expected = computeResponse(expectedPassword, nonce);
    if (response.size() != expected.size())
        return false;

    // 与 verifyRequest 同构的定长逐字节异或累积 —— 不提前返回，避免时序侧信道。
    unsigned char diff = 0;
    for (qsizetype i = 0; i < expected.size(); ++i)
        diff |= static_cast<unsigned char>(response.at(i) ^ expected.at(i));
    return diff == 0;
}

bool verifyRequest(const QByteArray &requestPayload, const QString &expectedPassword)
{
    const QByteArray expected = hashPassword(expectedPassword);
    if (requestPayload.size() != expected.size())
        return false;

    // 定长摘要逐字节异或累积，避免提前返回带来的时序差异。
    unsigned char diff = 0;
    for (qsizetype i = 0; i < expected.size(); ++i)
        diff |= static_cast<unsigned char>(requestPayload.at(i) ^ expected.at(i));
    return diff == 0;
}

} // namespace auth

AuthServer::AuthServer(QString password, QObject *parent)
    : QObject(parent)
    , password_(std::move(password))
{
}

void AuthServer::attach(Session *session)
{
    session_ = session;

    // 挑战-响应握手：服务端**先发** CHALLENGE。
    // 客户端连上后会发一帧空的 HandshakeReq 作「我来了」；但即便它没发，
    // 我们也在 attach 时立刻推 nonce —— 少一个往返、也少一个「等 REQ 超时」的状态。
    sendChallenge();

    connect(session_, &Session::frameReceived, this, [this](const Frame &f) {
        // 已经给过结论（OK / DENY）就不再受理 —— 同一条连接上重发响应
        // 不得让 authenticated / rejected 再触发一次（见头文件 done_ 的说明）。
        if (done_)
            return;

        if (f.type == static_cast<std::uint8_t>(FrameType::HandshakeReq)) {
            // 客户端声明 + 发起（载荷为空）。CHALLENGE 在 attach 时已经发过了。
            //
            // ⚠️ 这里**绝不能**再调用 sendChallenge()。CHALLENGE 已发出、客户端
            // 可能已经在拿它算响应了；再换一个 nonce 会让那份响应立刻作废 ——
            // 表现为「先收到一帧 DENY，紧接着又是 OK / DENY」，且完全看两端跑得多快，
            // 是个随机的握手失败。
            if (nonce_.isEmpty())
                sendChallenge();
            return;
        }

        if (f.type != static_cast<std::uint8_t>(FrameType::HandshakeResponse))
            return;

        // 从这里开始一定会给出结论，先落闩 —— 无论结论是 OK 还是 DENY。
        done_ = true;

        if (auth::verifyResponse(f.payload, password_, nonce_)) {
            // 口令正确，但容量等接受条件不满足时也按拒绝处理（在回 OK 之前判定）。
            if (acceptGuard_ && !acceptGuard_()) {
                session_->sendFrame(static_cast<std::uint8_t>(FrameType::HandshakeDeny));
                emit rejected(session_);
                return;
            }
            session_->sendFrame(static_cast<std::uint8_t>(FrameType::HandshakeOk));
            emit authenticated(session_);
        } else {
            session_->sendFrame(static_cast<std::uint8_t>(FrameType::HandshakeDeny));
            emit rejected(session_);
        }
    });
}

void AuthServer::sendChallenge()
{
    // 每轮都换新 nonce：重放防护的全部价值都压在这上面。
    nonce_ = auth::makeNonce();
    session_->sendFrame(static_cast<std::uint8_t>(FrameType::HandshakeChallenge), nonce_);
}

AuthClient::AuthClient(Session *session, QString password, QObject *parent)
    : QObject(parent)
    , session_(session)
    , password_(std::move(password))
{
}

void AuthClient::start()
{
    Q_ASSERT(session_ != nullptr);

    connect(session_, &Session::frameReceived, this, [this](const Frame &f) {
        if (done_)
            return;

        if (f.type == static_cast<std::uint8_t>(FrameType::HandshakeChallenge)) {
            // 拿到 nonce 才回响应 —— 客户端**不再**发送任何口令派生摘要作为「请求」，
            // 线上唯一与口令有关的东西就是这一次性 HMAC。
            //
            // 同一个 nonce 只回一次：服务端理论上只发一帧 CHALLENGE，但万一重复到达，
            // 回两份响应会让服务端先按第一份判、再把第二份当新请求，徒增歧义。
            if (f.payload == answeredNonce_)
                return;
            answeredNonce_ = f.payload;
            session_->sendFrame(static_cast<std::uint8_t>(FrameType::HandshakeResponse),
                                auth::computeResponse(password_, f.payload));
            return;
        }

        if (f.type == static_cast<std::uint8_t>(FrameType::HandshakeOk)) {
            done_ = true;
            emit succeeded();
        } else if (f.type == static_cast<std::uint8_t>(FrameType::HandshakeDeny)) {
            done_ = true;
            emit failed();
        }
    });

    connect(session_, &Session::disconnected, this, [this]() {
        if (done_)
            return;
        done_ = true;
        emit failed();
    });

    // 声明 + 发起（载荷为空）。真正的凭据在收到 CHALLENGE 之后才产生。
    session_->sendFrame(static_cast<std::uint8_t>(FrameType::HandshakeReq));
}

} // namespace lanrdp
