#include "net/FileSender.h"

#include "net/Session.h"
#include "protocol/FileTransfer.h"
#include "protocol/Frame.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QTcpSocket>

#include <utility>

namespace lanrdp {

namespace {

// 写缓冲高水位：超过就暂停，等 bytesWritten 再继续
constexpr qint64 kWriteHighWater = 4 * 1024 * 1024;

// 单次 pump 的发送预算：用完主动让出事件循环（否则传大文件会把 UI 卡死）
constexpr qint64 kPumpBudgetBytes = 4 * 1024 * 1024;

} // namespace

FileSender::FileSender(Session *session, QObject *parent)
    : QObject(parent)
    , session_(session)
{
}

FileSender::~FileSender() = default;

void FileSender::ensureConnections()
{
    if (connected_ || !session_ || !session_->socket())
        return;

    QTcpSocket *socket = session_->socket();

    connect(socket, &QTcpSocket::bytesWritten, this, [this](qint64) {
        if (state_ == State::Sending)
            pump();
    });

    connect(socket, &QTcpSocket::disconnected, this, [this]() {
        if (state_ == State::Sending)
            conclude(false, QStringLiteral("传输中连接断开"));
    });

    connected_ = true;
}

bool FileSender::start(const QString &path, std::uint32_t transferId)
{
    if (state_ == State::Sending)
        conclude(false, QStringLiteral("上一次传输尚未结束，已中止"));

    path_ = path;

    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly)) {
        const QString why = file->errorString();
        conclude(false, QStringLiteral("无法读取文件：%1").arg(why));
        return false;
    }

    const qint64 size = file->size();
    if (size <= 0) {
        conclude(false, QStringLiteral("空文件或无法确定大小"));
        return false;
    }

    if (!session_ || !session_->socket()) {
        conclude(false, QStringLiteral("没有可用的连接"));
        return false;
    }

    name_       = QFileInfo(path).fileName();
    transferId_ = transferId;
    totalSize_  = static_cast<quint64>(size);
    sent_       = 0;
    sha_.clear();
    file_       = std::move(file);
    hash_       = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    state_      = State::Sending;
    scheduled_  = false;

    ensureConnections();

    FileHeader header;
    header.transferId = transferId_;
    header.name       = name_;
    header.totalSize  = totalSize_;

    const QByteArray payload = encodeFileHeader(header);
    if (payload.isEmpty()) {
        conclude(false, QStringLiteral("无法编码 FILE_HEADER"));
        return false;
    }
    session_->sendFrame(static_cast<quint8>(FrameType::FileHeader), payload);

    emit started(name_, totalSize_);
    pump();
    return true;
}

void FileSender::abort()
{
    if (state_ != State::Sending)
        return;
    conclude(false, QStringLiteral("发送被中止"));
}

void FileSender::pump()
{
    if (state_ != State::Sending)
        return;
    scheduled_ = false;

    QTcpSocket *socket = session_ ? session_->socket() : nullptr;
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        conclude(false, QStringLiteral("连接已断开"));
        return;
    }

    qint64 budget = kPumpBudgetBytes;

    while (budget > 0 && socket->bytesToWrite() < kWriteHighWater) {
        const QByteArray block = file_->read(kFileChunkSize);
        if (block.isEmpty()) {
            // 读空**不**等于「正常读完」。
            // 源文件可能在中途被改小 / 读取出错（另一进程改写、设备错误）。
            // 先问 QFile 自己有没有报错，再核对「已发字节数 == 开始时记下的总长」；
            // 两者都过不去才算真的读完了。否则会照常发 FILE_END 说「发送完成」，
            // 而实际只发了一半（接收端能兜住，但那是**对方**在替我们发现）。
            if (file_->error() != QFile::NoError) {
                conclude(false, QStringLiteral("读取源文件失败：%1").arg(file_->errorString()));
                return;
            }
            if (sent_ != totalSize_) {
                conclude(false, QStringLiteral("源文件在传输中变了（声明 %1 字节，实发 %2 字节）")
                                    .arg(totalSize_)
                                    .arg(sent_));
                return;
            }

            // 文件读完 -> 发 FILE_END 收尾
            sha_ = hash_->result();

            FileEnd end;
            end.transferId = transferId_;
            end.bytesSent  = sent_;
            end.sha256     = sha_;

            const QByteArray endPayload = encodeFileEnd(end);
            if (endPayload.isEmpty()) {
                conclude(false, QStringLiteral("无法编码 FILE_END"));
                return;
            }
            session_->sendFrame(static_cast<quint8>(FrameType::FileEnd), endPayload);
            conclude(true, QStringLiteral("发送完成"));
            return;
        }

        FileChunk chunk;
        chunk.transferId = transferId_;
        chunk.offset     = sent_;
        chunk.data       = block;

        const QByteArray chunkPayload = encodeFileChunk(chunk);
        if (chunkPayload.isEmpty()) {
            conclude(false, QStringLiteral("无法编码 FILE_CHUNK"));
            return;
        }

        session_->sendFrame(static_cast<quint8>(FrameType::FileChunk), chunkPayload);
        hash_->addData(block);
        sent_ += static_cast<quint64>(block.size());
        budget -= block.size();

        emit progress(sent_, totalSize_);
    }

    // 预算用完（不是被水位挡住）-> 让出事件循环，稍后接着发。
    // 若是因为写缓冲满而退出循环，则等 bytesWritten 唤醒，不在这里排队自旋。
    if (budget <= 0)
        scheduleNextPump();
}

void FileSender::scheduleNextPump()
{
    if (scheduled_ || state_ != State::Sending)
        return;
    scheduled_ = true;
    QMetaObject::invokeMethod(this, [this]() { pump(); }, Qt::QueuedConnection);
}

void FileSender::conclude(bool ok, const QString &message)
{
    const QString path = path_;

    state_      = State::Idle;
    scheduled_  = false;
    transferId_ = 0;

    if (file_) {
        file_->close();
        file_.reset();
    }
    hash_.reset();

    // 注意：不清空 name_ / totalSize_ / sent_ / sha_。
    // 传输结束后这些仍是「上一笔」的有效信息，UI 要靠它们显示结果；
    // 是否正在进行中用 busy() 判断，而不是看计数是否为 0。
    emit finished(ok, path, message);
}

} // namespace lanrdp
