#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>

class QFile;
class QCryptographicHash;

namespace lanrdp {

class Session;

// 把一个本地文件分块发往对端。
//
// 背压：socket 写缓冲超过高水位就暂停、等 bytesWritten 再继续，所以内存占用
// 与文件大小无关（约 4 MiB + 一块）。单次 pump 还有发送预算，用完会主动让出
// 事件循环，避免传大文件时把界面卡住。
class FileSender : public QObject
{
    Q_OBJECT

public:
    explicit FileSender(Session *session, QObject *parent = nullptr);
    ~FileSender() override;

    // 开始发送。返回 false 表示文件不可读（同时会 emit 一次 finished(false, …)）。
    bool start(const QString &path, std::uint32_t transferId);

    // 主动中止（连接销毁前调用），会 emit 一次 finished(false, …)。
    void abort();

    bool       busy() const { return state_ == State::Sending; }
    QString    fileName() const { return name_; }
    quint64    totalBytes() const { return totalSize_; }
    quint64    sentBytes() const { return sent_; }
    QByteArray sha256() const { return sha_; }  // 发送完成后可用

signals:
    void started(const QString &fileName, quint64 totalSize);
    void progress(quint64 sent, quint64 total);
    void finished(bool ok, const QString &path, const QString &message);

private:
    enum class State
    {
        Idle,
        Sending,
    };

    void ensureConnections();
    void pump();              // 在不超水位的前提下尽量多发包
    void scheduleNextPump();  // 排队一次 pump，把控制权还给事件循环
    void conclude(bool ok, const QString &message);

    Session                            *session_ = nullptr;
    State                               state_    = State::Idle;
    bool                                connected_ = false;
    bool                                scheduled_ = false;
    QString                             path_;
    QString                             name_;
    std::uint32_t                       transferId_ = 0;
    quint64                             totalSize_  = 0;
    quint64                             sent_       = 0;
    QByteArray                          sha_;
    std::unique_ptr<QFile>              file_;
    std::unique_ptr<QCryptographicHash> hash_;
};

} // namespace lanrdp
