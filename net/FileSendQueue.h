#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace lanrdp {

class Session;
class FileSender;

// 把一批文件发给**一条会话**的串行队列。
//
// 抽出来独立成类，是因为队列里压着两条不稳的约束 —— 复制一份到第二个调用方，
// 它们迟早会各自漂移：
// ① `FileSender` 同一时刻只能传一份（撞上去会中止前一个），所以多份必须排队，
//    不能连环 start()；
// ② 一份失败就**整批放弃**。对端按剪贴板清单凑批，缺一个就永远等不齐 ——
//    「跳过一个继续传」看着更宽容，实际是把对端挂在「接收中」直到超时。
//
// 两个方向（控制端 → 被控端、被控端 → 控制端）共用这一个实现。
class FileSendQueue : public QObject
{
    Q_OBJECT

public:
    explicit FileSendQueue(QObject *parent = nullptr);
    ~FileSendQueue() override;

    // 绑定（或解绑，传 nullptr）发送目标的会话。换会话即中止在途那份 ——
    // 上一批没发完的文件不该跟着跑到另一台机器上去。
    void     setSession(Session *session);
    Session *session() const { return session_; }

    // 传输 id 的**下一个**取值。起点随机、绑定的会话内递增（判据只用「不重复」）。
    std::uint32_t nextTransferId() const { return nextTransferId_; }

    // 逐个发，不带清单。已有在途的那份不会被打断，追加到队尾。
    bool enqueue(const QStringList &paths);

    // 先发剪贴板文件清单（CLIPBOARD_FILES）再逐个发：对端据此知道「要凑几个」。
    bool enqueueClipboardBatch(const QStringList &paths);

    bool busy() const;
    int  pendingCount() const { return int(queue_.size()); }

    // 中止在途那份并清空队列（不留半批）。会话要没了 / 用户主动断开时调用。
    void abort();

signals:
    void started(const QString &fileName, quint64 total);
    void progress(quint64 sent, quint64 total);
    void finished(bool ok, const QString &path, const QString &message);
    void logMessage(const QString &text);

private:
    void ensureSender();
    void pump();

    Session     *session_ = nullptr;
    FileSender  *sender_  = nullptr;
    // 等着的文件（本地绝对路径），队首就是下一个要 start() 的。
    QStringList  queue_;
    std::uint32_t nextTransferId_ = 1;
};

} // namespace lanrdp
