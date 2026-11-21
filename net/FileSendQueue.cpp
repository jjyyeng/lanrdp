#include "net/FileSendQueue.h"

#include "net/FileSender.h"
#include "net/Session.h"
#include "protocol/Clipboard.h"
#include "protocol/Frame.h"

#include <QFileInfo>
#include <QRandomGenerator>

namespace lanrdp {

FileSendQueue::FileSendQueue(QObject *parent)
    : QObject(parent)
{
    // 传输 id 从随机值起步：它是协议里的唯一标识，固定从 1 开始的话多条连接会
    // 撞在同一串 id 上。取值只要求同一条会话内不重复。
    nextTransferId_ = QRandomGenerator::global()->generate();
}

FileSendQueue::~FileSendQueue() = default;

void FileSendQueue::setSession(Session *session)
{
    if (session_ == session)
        return;

    // 换会话：在途那份与队列里的都不该继续（目标已经不是同一台机器了）。
    // `FileSender` 绑死在旧 Session 上，必须销毁重建，不能改绑。
    abort();
    if (sender_) {
        delete sender_;
        sender_ = nullptr;
    }
    session_ = session;
}

void FileSendQueue::ensureSender()
{
    if (sender_ || !session_)
        return;

    sender_ = new FileSender(session_, this);
    connect(sender_, &FileSender::started, this, &FileSendQueue::started);
    connect(sender_, &FileSender::progress, this, &FileSendQueue::progress);
    connect(sender_, &FileSender::finished, this,
            [this](bool ok, const QString &path, const QString &message) {
                emit finished(ok, path, message);
                if (ok) {
                    pump();   // 上一份传完了，接着传队列里的下一份
                    return;
                }

                // 一份失败就整批放弃。只在**还有剩的**时候报一句：
                // 队列本来就只有一个文件时，失败原因已经由 finished 报过了。
                if (!queue_.isEmpty()) {
                    const int left = int(queue_.size());
                    queue_.clear();
                    emit logMessage(QStringLiteral("已放弃队列中剩余的 %1 个文件").arg(left));
                }
            });
}

bool FileSendQueue::enqueue(const QStringList &paths)
{
    if (paths.isEmpty() || !session_)
        return false;

    ensureSender();
    if (!sender_)
        return false;

    queue_.append(paths);
    pump();
    return true;
}

bool FileSendQueue::enqueueClipboardBatch(const QStringList &paths)
{
    if (paths.isEmpty() || !session_)
        return false;

    if (paths.size() > kClipboardMaxFiles) {
        emit logMessage(QStringLiteral("剪贴板里的文件没有发出去：一次最多 %1 个，这次有 %2 个")
                            .arg(kClipboardMaxFiles)
                            .arg(paths.size()));
        return false;
    }

    QStringList names;
    names.reserve(paths.size());
    for (const QString &path : paths)
        names.append(QFileInfo(path).fileName());

    const auto payload = encodeClipboardFiles(names);
    if (!payload) {
        emit logMessage(QStringLiteral("剪贴板里的文件没有发出去：这批文件名过长或为空"));
        return false;
    }

    // 顺序要紧：清单先走（TCP 有序），对端先知道「要凑几个」，再收文件。
    Frame manifest;
    manifest.type    = static_cast<std::uint8_t>(FrameType::ClipboardFiles);
    manifest.payload = *payload;
    session_->sendFrame(manifest);

    return enqueue(paths);
}

bool FileSendQueue::busy() const
{
    return sender_ && sender_->busy();
}

void FileSendQueue::abort()
{
    queue_.clear();
    if (sender_)
        sender_->abort();
}

void FileSendQueue::pump()
{
    if (!sender_ || queue_.isEmpty())
        return;
    if (sender_->busy())
        return;   // 上一份还在传：它的 finished 会再叫一次

    const QString next = queue_.takeFirst();
    if (!sender_->start(next, nextTransferId_++)) {
        // start() 自己已经 conclude 过并**同步** emit 了 finished(false) ——
        // 「整批放弃」与那句日志都在 finished 的失败分支里做掉了。
        queue_.clear();
    }
}

} // namespace lanrdp
