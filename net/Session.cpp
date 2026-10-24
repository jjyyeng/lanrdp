#include "net/Session.h"

#include <QTcpSocket>
#include <QTimer>

#include <atomic>

namespace lanrdp {

quint64 Session::nextId()
{
    // 取号器只增不减：地址会被复用，号不会。
    static std::atomic<quint64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

Session::Session(QTcpSocket *socket, QObject *parent)
    : QObject(parent)
    , socket_(socket)
{
    Q_ASSERT(socket_ != nullptr);
    socket_->setParent(this);

    // 关掉 Nagle（TCP_NODELAY）。
    //
    // 这是个**交互式**协议：令牌请求 / 输入事件 / 心跳都是几十字节的小帧，而 Nagle 会
    // 在有未确认数据时把小段压住等 ACK —— 推流期间「被控端→控制端」方向一直在发大帧，
    // 正好构成那个「有未确认数据」的条件，于是输入类小帧被反复延后。
    //
    // 实测（推流期间 `TokenReq -> TokenGrant` 往返，25 样本/轮，本机 loopback）：
    // 最大往返从 171–266 ms 降到 94–172 ms；中位基本不变（~45 ms，那是宿主线程里
    // 采集/编码的耗时，与 Nagle 无关 ⇒ 别把这条当成「延迟都修好了」）。
    //
    // 两端都要设：本类被被控端（accept 出来的 socket）与控制端（主动 connect 的 socket）
    // 共用，这里一次覆盖两边。
    socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    connect(socket_, &QTcpSocket::readyRead, this, &Session::onReadyRead);
    connect(socket_, &QTcpSocket::disconnected, this, &Session::onDisconnected);
}

void Session::sendFrame(const Frame &frame)
{
    if (!socket_)
        return;

    const QByteArray wire = frame.serialize();
    if (wire.isEmpty()) {
        // 载荷超限（见 kMaxPayloadLen）。发不出去的帧只能丢 —— 硬发会让
        // 对端按错误长度切帧、判流损坏后断连，那种现场比丢帧更难排查。
        if (!oversizeWarned_) {
            oversizeWarned_ = true;
            qWarning("lanrdp: 帧载荷 %lld 字节超过协议上限 %u，本帧已丢弃（仅提示一次）",
                     static_cast<long long>(frame.payload.size()),
                     static_cast<unsigned>(kMaxPayloadLen));
        }
        return;
    }
    socket_->write(wire);
}

void Session::sendFrame(std::uint8_t type, const QByteArray &payload)
{
    Frame f;
    f.type    = type;
    f.payload = payload;
    sendFrame(f);
}

void Session::onReadyRead()
{
    if (!socket_)
        return;

    const QByteArray chunk = socket_->readAll();

    // 缓冲里可能还压着上一轮没处理完的帧（见 parseBufferedFrames 的预算），
    // 所以「这一次没读到新字节」不等于「没事可做」。
    if (chunk.isEmpty() && rxbuf_.isEmpty())
        return;

    if (!chunk.isEmpty()) {
        rxbuf_.append(chunk);

        // 任何字节到达都算「对端还活着」，不只是完整帧 ——
        // 大文件传输中间的长帧也不会被误判成死链。
        lastRxTimer_.restart();
        hasRx_ = true;
    }

    // 兜底：即使 Frame::tryParse 因为将来加入新的长度语义而
    // 放宽了校验，这里也独立设一道门槛 —— 接收缓冲不得无界增长。
    if (rxbuf_.size() - readOffset_ > qsizetype(kMaxPayloadLen) + qsizetype(kHeaderSize)) {
        rxbuf_.clear();
        readOffset_ = 0;
        socket_->disconnectFromHost();
        return;
    }

    parseBufferedFrames();
}

// 只消耗**已经到达**的完整帧，不再读 socket。之所以从 onReadyRead 里拆出来：
// 单轮预算耗尽后，剩下的帧必须在对端不再发字节时也能被处理 —— 原来一 break 就
// 退出，得等下一次 readAll() 非空才回得来；对端推完一屏静止画面就停发，于是画面
// 停在那里不动，一直挂到心跳把连接判死。
void Session::parseBufferedFrames()
{
    if (!socket_)
        return;

    // 偏移分帧 —— 只推进 readOffset_，不再每帧 remove(0,n)
    // 整体搬移（那会让 N 字节里塞 s 字节小帧的总搬移量达到 N²/(2s)）。
    // 同时给单轮的分帧循环设一个预算：正常情况下排空即退出，
    // 预算只是防止「槽函数同步灌入新数据」造成的无限循环。
    constexpr int kMaxFramesPerRound = 100000;
    int dispatched = 0;

    for (;;) {
        if (dispatched >= kMaxFramesPerRound)
            break;

        Frame f;
        qsizetype consumed = 0;
        const ParseStatus st = Frame::tryParse(rxbuf_, &f, &consumed, readOffset_);

        if (st == ParseStatus::Ok) {
            readOffset_ += consumed;
            ++dispatched;
            emit frameReceived(f);
            continue;
        }

        if (st == ParseStatus::Invalid) {
            // 流已损坏（含荒唐帧长）：清空缓冲并主动断开
            rxbuf_.clear();
            readOffset_ = 0;
            socket_->disconnectFromHost();
        }

        break; // Incomplete：等更多字节 / Invalid：已请求断开
    }

    // 一次性压缩：把已消费的前缀真正丢弃。
    if (readOffset_ > 0) {
        rxbuf_.remove(0, readOffset_);
        readOffset_ = 0;
    }

    // 预算耗尽 ⇒ 缓冲里还有帧没排完，让出一次事件循环再接着排。
    // 不能在这里同步续排：frameReceived 的接收方可能顺手灌进新数据，
    // 同步续排会一直霸着事件循环，心跳与界面全都停摆。
    if (dispatched >= kMaxFramesPerRound)
        QTimer::singleShot(0, this, [this] { parseBufferedFrames(); });
}

qint64 Session::msSinceLastRx() const
{
    if (!hasRx_ || !lastRxTimer_.isValid())
        return kNeverReceivedMs;
    return lastRxTimer_.elapsed();
}

qint64 Session::pendingBytes() const
{
    return socket_ ? socket_->bytesToWrite() : 0;
}

void Session::onDisconnected()
{
    emit disconnected();
}

} // namespace lanrdp
