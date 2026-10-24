#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>

#include <limits>

#include "protocol/Frame.h"

class QTcpSocket;

namespace lanrdp {

// 一条 TCP 连接 = 一个 Session（核心抽象）。
// 负责把字节流分帧成 Frame，并提供发帧。
// 承载 1:1 全链路；被控端侧多条由 `HostServer::sessions_` 持有，控制端侧由 `ClientConnection` 持有。
class Session : public QObject
{
    Q_OBJECT

public:
    // 接管 socket 所有权（重设为自身子对象）。
    explicit Session(QTcpSocket *socket, QObject *parent = nullptr);

    QTcpSocket *socket() const { return socket_; }

    // 会话身份：进程内单调递增、唯一。
    // 跨线程 / 跨快照传它而**不传裸指针** —— 会话离开池后会被释放，新连接又可能
    // 恰好落在同一块地址（ABA），那时按指针回查会踢错人。
    quint64 id() const { return id_; }

    void sendFrame(const Frame &frame);
    void sendFrame(std::uint8_t type, const QByteArray &payload = QByteArray());

    // 距离「最后一次收到对端字节」过去了多少毫秒。
    // 从未收到过任何字节时返回 kNeverReceivedMs。
    //
    // ⚠️ 这个哨兵**不能**直接拿去和死链阈值比大小 —— 它远大于任何阈值，一比就把
    // 「刚建好、还没来得及收」判成死链（心跳的**首个** tick 就会把连接判死）。
    // 判死链请配合 hasReceived()：收过字节才拿本值当计时，没收过就用「本次尝试
    // 已过去多久」。见 ClientConnection::isDeadLink。
    qint64 msSinceLastRx() const;

    // 是否至少收到过一字节 —— 用来给「静默了多久」挑计时起点（见上）。
    bool hasReceived() const { return hasRx_; }

    // 自适应码率：发送队列积压 = socket 还没写出去的字节数。
    // 链路慢（对端接收慢）时该值会涨；自适应码率据此判「卡了」并降码率。
    qint64 pendingBytes() const;

    static constexpr qint64 kNeverReceivedMs = std::numeric_limits<qint64>::max();

signals:
    void frameReceived(const lanrdp::Frame &frame);
    void disconnected();

private slots:
    void onReadyRead();
    void onDisconnected();

private:
    // 只消耗 rxbuf_ 里**已经到达**的完整帧，不再读 socket。
    // 独立成函数是为了让「单轮预算耗尽」能自己再排一轮（见实现里的说明）。
    void parseBufferedFrames();

    QTcpSocket *socket_ = nullptr;
    QByteArray  rxbuf_;

    // 已从 rxbuf_ 前端消费掉、但尚未物理丢弃的字节数。
    // 旧的 rxbuf_.remove(0, consumed) 每帧都要整体前移剩余字节，N 字节里塞
    // s 字节的小帧时总搬移量约 N²/(2s)（最坏放大数千倍）—— 一次小帧洪峰
    // 就能把 UI 线程冻住。改为「只推进下标、循环结束后压缩一次」。
    qsizetype readOffset_ = 0;

    // 收帧时刻基准。用 QElapsedTimer（单调时钟）而不是 QDateTime，
    // 避免系统时间被校准 / 跨时区调整时把死链判定带偏。
    QElapsedTimer lastRxTimer_;
    bool          hasRx_ = false;

    // 「载荷超过协议上限、已丢帧」只提示一次：超限是配置性问题（分辨率过高），
    // 每帧刷一行会把日志冲垮，反而盖掉真正有用的记录。
    bool oversizeWarned_ = false;

    // 取号器（实现里是一个静态原子量，进程内唯一）。
    static quint64 nextId();
    const quint64   id_ = nextId();
};

} // namespace lanrdp
