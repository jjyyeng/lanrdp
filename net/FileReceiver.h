#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <unordered_map>

class QFile;
class QTimer;
class QCryptographicHash;

namespace lanrdp {

struct Frame;

// 接收入站 FILE_* 帧并落盘。
//
// **状态按来源隔离** —— 被控端可能同时有多个控制端在传文件，一个进程里
// 只有一个接收器，但它内部为每个来源各存一份传输状态。于是：
//   · 后到的 HEADER 不会打断别人正在进行的传输；
//   · 某条连接断开只 abort 它自己那一份。
//
// 「来源」是一个**不透明标识**（`Source`）：生产代码传 `Session*`，
// 测试可以传任意非空指针。接收器只把它当哈希键用，从不解引用。
//
// 用法：把每条收到的 Frame 连同**它的来源**喂给 onFrame(source, frame)；
// 一次传输结束时（成功、校验失败或 abort）会 emit 恰好一次
// finished(ok, path, message)。对端可以发来任意文件名，落盘前一律经过
// sanitizeFileName + 接收目录内去重。
class FileReceiver : public QObject
{
    Q_OBJECT

public:
    // 传输来源的标识。nullptr = 「无来源」，一律忽略。
    using Source = const void *;

    explicit FileReceiver(QString downloadDir, QObject *parent = nullptr);
    ~FileReceiver() override;

    void    setDownloadDir(const QString &dir);
    QString downloadDir() const { return downloadDir_; }

    // 单个入站文件的尺寸上限。默认 4 GiB —— 足够局域网内传大镜像，
    // 又不至于让一个被攻陷的对端一路把磁盘写满。0 表示不限制。
    void    setMaxFileSize(quint64 bytes) { maxFileSize_ = bytes; }
    quint64 maxFileSize() const { return maxFileSize_; }

    // 单笔传输的「无数据块」超时。对端发来 HEADER 之后就不再发块（却照常回心跳，
    // 死链判定把任何字节都当活性）时，靠它把这一笔收掉 —— 否则文件句柄被长期占住、
    // 磁盘上留一个 0 字节目标文件、UI 停在「接收中」，而**下一次正常传输还会静默
    // abort 它**（同一来源来了新 HEADER，旧的那笔被判「被新的传输打断」）。
    // 0 = 关闭超时（不建议：那是本缺陷原本的状态）。
    void setTransferTimeoutMs(int ms) { transferTimeoutMs_ = ms; }
    int  transferTimeoutMs() const { return transferTimeoutMs_; }

    static constexpr int kDefaultTransferTimeoutMs = 30 * 1000;

    // 某个来源是否正在收文件。传 nullptr 时问「有没有任何一路在收」。
    bool    busy(Source from = nullptr) const;
    // 正在接收的文件数（并行路数）。
    int     activeCount() const;

    // 内部记为「这个来源」的条目数（含已收完、状态已回 Idle 的）。
    // 收完即 erase ⇒ 正常应恒等于 activeCount()；主要是给测试盯住「条目不回收」的回归。
    int     trackedTransfers() const { return static_cast<int>(transfers_.size()); }

    // 下面几个是针对**单个来源**的查询；传 nullptr 时返回「最近活跃的那一路」，
    // 供只有一个接收方时的 UI 直接使用。
    QString currentFileName(Source from = nullptr) const;
    quint64 receivedBytes(Source from = nullptr) const;
    quint64 totalBytes(Source from = nullptr) const;

    // 喂入一帧。返回 false 表示这帧不是 FILE_*，调用方应自行处理。
    // `from` 为 nullptr 时按「无来源」处理（忽略，不误伤在跑的传输）。
    bool onFrame(Source from, const Frame &frame);

    // 丢弃半截文件，避免留下骗人的残file。
    // 传 nullptr 清掉**全部**（停机路径）；传来源只清它那一份（断连路径）。
    void abort(Source from = nullptr);

signals:
    // started / progress 带来源，调用方据此区分是哪一路。
    // ⚠️ finished **不**带来源（签名稳定，改了要动 HostController 与 5 处测试连接）：
    // 多路并行时若真需要区分，按 path 反查即可 —— 落盘路径本来就唯一。
    void started(lanrdp::FileReceiver::Source from, const QString &fileName, quint64 totalSize);
    void progress(lanrdp::FileReceiver::Source from, quint64 received, quint64 total);
    void finished(bool ok, const QString &path, const QString &message);

private:
    enum class State
    {
        Idle,
        Receiving,
    };

    // 一条来源的传输状态。
    struct Transfer
    {
        State                               state = State::Idle;
        QString                             name;        // 清洗去重后的文件名
        QString                             targetPath;
        quint32                             transferId = 0;
        quint64                             totalSize  = 0;
        quint64                             received   = 0;
        std::unique_ptr<QFile>              file;
        std::unique_ptr<QCryptographicHash> hash;
        // 无数据块超时：每笔传输一份，收到块就重新计时；conclude 时停掉。
        std::unique_ptr<QTimer>             timeout;
    };

    void handleHeader(Source from, const QByteArray &payload);
    void handleChunk(Source from, const QByteArray &payload);
    void handleEnd(Source from, const QByteArray &payload);

    // （重新）武装某来源的无块超时；transferTimeoutMs_ <= 0 时不动作。
    void armTransferTimeout(Source from);

    // 收起某来源的传输：关文件、失败时删掉，然后 emit finished。
    void conclude(Source from, bool ok, const QString &message);

    // 最近被写入的那一路（供无来源查询落到「当前显示的那一路」）。
    Source                                  lastActive_ = nullptr;
    // ⚠️ 用 std::unordered_map 而非 QHash：Transfer 持有 QFile /
    // QCryptographicHash，既不可拷贝也不可移动，而 Qt6 的 QHash 在扩容时
    // 要求 value 可拷贝。std::unordered_map 以 node 为单位分配，无此要求。
    std::unordered_map<Source, std::unique_ptr<Transfer>> transfers_;
    QString                                 downloadDir_;
    quint64                                 maxFileSize_ = 4ull * 1024 * 1024 * 1024;
    int                                     transferTimeoutMs_ = kDefaultTransferTimeoutMs;
};

} // namespace lanrdp
