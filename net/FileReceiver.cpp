#include "net/FileReceiver.h"

#include "protocol/FileTransfer.h"
#include "protocol/Frame.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTimer>

#include <utility>

namespace lanrdp {

FileReceiver::FileReceiver(QString downloadDir, QObject *parent)
    : QObject(parent)
    , downloadDir_(std::move(downloadDir))
{
}

FileReceiver::~FileReceiver()
{
    // 析构期间不 emit 信号，只做清理
    for (auto &entry : transfers_) {
        Transfer &t = *entry.second;
        if (t.file) {
            t.file->close();
            t.file.reset();
            if (!t.targetPath.isEmpty())
                QFile::remove(t.targetPath);
        }
    }
    transfers_.clear();
}

void FileReceiver::setDownloadDir(const QString &dir)
{
    downloadDir_ = dir;
}

bool FileReceiver::busy(Source from) const
{
    if (from) {
        const auto it = transfers_.find(from);
        return it != transfers_.end() && it->second && it->second->state == State::Receiving;
    }
    for (const auto &entry : transfers_) {
        if (entry.second && entry.second->state == State::Receiving)
            return true;
    }
    return false;
}

int FileReceiver::activeCount() const
{
    int n = 0;
    for (const auto &entry : transfers_) {
        if (entry.second && entry.second->state == State::Receiving)
            ++n;
    }
    return n;
}

QString FileReceiver::currentFileName(Source from) const
{
    const auto it = transfers_.find(from ? from : lastActive_);
    const Transfer *t = (it != transfers_.end()) ? it->second.get() : nullptr;
    return t ? t->name : QString();
}

quint64 FileReceiver::receivedBytes(Source from) const
{
    const auto it = transfers_.find(from ? from : lastActive_);
    const Transfer *t = (it != transfers_.end()) ? it->second.get() : nullptr;
    return t ? t->received : 0;
}

quint64 FileReceiver::totalBytes(Source from) const
{
    const auto it = transfers_.find(from ? from : lastActive_);
    const Transfer *t = (it != transfers_.end()) ? it->second.get() : nullptr;
    return t ? t->totalSize : 0;
}

bool FileReceiver::onFrame(Source from, const Frame &frame)
{
    switch (static_cast<FrameType>(frame.type)) {
    case FrameType::FileHeader:
        handleHeader(from, frame.payload);
        return true;
    case FrameType::FileChunk:
        handleChunk(from, frame.payload);
        return true;
    case FrameType::FileEnd:
        handleEnd(from, frame.payload);
        return true;
    default:
        return false;
    }
}

void FileReceiver::armTransferTimeout(Source from)
{
    if (transferTimeoutMs_ <= 0)
        return;

    auto it = transfers_.find(from);
    if (it == transfers_.end() || !it->second)
        return;

    Transfer &t = *it->second;
    if (!t.timeout) {
        t.timeout = std::make_unique<QTimer>();
        t.timeout->setSingleShot(true);
        // 捕获「的来源」而不是 Transfer 引用 —— 容器可能重新哈希 / 条目可能被
        // 复用，只有 Source 键是稳定的。conclude() 自身对「已经结束的那笔」幂等，
        // 所以即便定时器晚一拍才到也不会重复 emit。
        QObject::connect(t.timeout.get(), &QTimer::timeout, this, [this, from] {
            conclude(from, false,
                     QStringLiteral("接收超时（%1 ms 内没有收到数据块）").arg(transferTimeoutMs_));
        });
    }
    t.timeout->start(transferTimeoutMs_);
}

void FileReceiver::handleHeader(Source from, const QByteArray &payload)
{
    // 没有来源的帧一律忽略 —— 没有来源就无法隔离状态，
    // 硬收下来只会污染某一路（宁可丢弃）。
    if (!from)
        return;

    auto &slot = transfers_[from];
    if (!slot)
        slot = std::make_unique<Transfer>();
    Transfer &t = *slot;

    // 这个来源**自己**上一笔还没收完就又来个 HEADER：把旧的按失败收掉。
    // ⚠️ 只收掉自己那一份 —— 别的控制端正传着的文件不受影响。
    if (t.state == State::Receiving)
        conclude(from, false, QStringLiteral("被新的传输打断"));

    const auto header = decodeFileHeader(payload);
    if (!header) {
        emit finished(false, QString(), QStringLiteral("FILE_HEADER 载荷非法"));
        return;
    }

    // 两道准入检查都放在**建文件之前**：宁可一个字节都不收，
    // 也不要先落一个注定写不完的残file。
    if (maxFileSize_ > 0 && header->totalSize > maxFileSize_) {
        emit finished(false, QString(),
                      QStringLiteral("文件超过接收上限（%1 字节 > %2 字节）")
                          .arg(header->totalSize)
                          .arg(maxFileSize_));
        return;
    }

    QDir dir(downloadDir_);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        emit finished(false, QString(),
                      QStringLiteral("接收目录不可用：%1").arg(downloadDir_));
        return;
    }

    // 声明尺寸再大也大不过磁盘剩余空间 —— 提前拒，避免收一半才 ENOSPC。
    const QStorageInfo storage(dir.absolutePath());
    if (storage.isValid() && storage.bytesAvailable() >= 0
        && header->totalSize > quint64(storage.bytesAvailable())) {
        emit finished(false, QString(),
                      QStringLiteral("接收目录剩余空间不足（需要 %1 字节，可用 %2 字节）")
                          .arg(header->totalSize)
                          .arg(storage.bytesAvailable()));
        return;
    }

    // 文件名来自对端，必须清洗 + 去重后才允许落盘
    const QString safe = sanitizeFileName(header->name);
    const QString unique =
        uniquifyFileName(safe, [&dir](const QString &candidate) {
            const QFileInfo info(dir.absoluteFilePath(candidate));
            // 悬空符号链接：exists() 返回 false，但 open(NewOnly) 会跟随它在**目录之外**
            // 建文件。把链接一律当作「已占用」，让候选名跳过它（防御纵深）。
            return info.exists() || info.isSymLink();
        });

    // 候选名耗尽时 uniquifyFileName 返回空 —— 必须失败，
    // 绝不能回落到原名（那会 Truncate 掉用户已有的同名文件）。
    if (unique.isEmpty()) {
        emit finished(false, QString(),
                      QStringLiteral("同名文件过多，无法为 %1 找到可用文件名").arg(safe));
        return;
    }

    const QString path = dir.absoluteFilePath(unique);

    // 落盘用 NewOnly —— 即便上面的去重被并发抢先，
    // 也只可能**失败**，绝不会截断一个已有文件。
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        emit finished(false, path,
                      QStringLiteral("无法写入接收目录：%1").arg(file->errorString()));
        return;
    }

    t.transferId = header->transferId;
    t.name       = unique;
    t.targetPath = path;
    t.totalSize  = header->totalSize;
    t.received   = 0;
    t.file       = std::move(file);
    t.hash       = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    t.state      = State::Receiving;
    lastActive_  = from;
    armTransferTimeout(from);   // 开了文件就开始计时：对端不发块也不能把句柄占死

    emit started(from, t.name, t.totalSize);
}

void FileReceiver::handleChunk(Source from, const QByteArray &payload)
{
    if (!from)
        return;

    auto it = transfers_.find(from);
    if (it == transfers_.end() || !it->second)
        return;  // 这个来源没有进行中的传输

    Transfer &t = *it->second;
    if (t.state != State::Receiving)
        return;  // 没有 HEADER 的孤儿块，忽略

    const auto chunk = decodeFileChunk(payload);
    if (!chunk || chunk->transferId != t.transferId)
        return;

    // offset 字段的价值就在这：sha 对不上时能立刻区分「丢块」还是「内容错」
    if (chunk->offset != t.received) {
        conclude(from, false, QStringLiteral("数据块不连续（期望偏移 %1，实到 %2）")
                                  .arg(t.received)
                                  .arg(chunk->offset));
        return;
    }

    if (t.received + static_cast<quint64>(chunk->data.size()) > t.totalSize) {
        conclude(from, false, QStringLiteral("收到的数据超过声明长度"));
        return;
    }

    if (t.file->write(chunk->data) != chunk->data.size()) {
        conclude(from, false, QStringLiteral("写盘失败：%1").arg(t.file->errorString()));
        return;
    }

    t.hash->addData(chunk->data);
    t.received += static_cast<quint64>(chunk->data.size());
    lastActive_ = from;
    armTransferTimeout(from);   // 每收到一块就重新计时（超时 = 「无块」而非「总时长」）
    emit progress(from, t.received, t.totalSize);
}

void FileReceiver::handleEnd(Source from, const QByteArray &payload)
{
    if (!from)
        return;

    auto it = transfers_.find(from);
    if (it == transfers_.end() || !it->second)
        return;

    Transfer &t = *it->second;
    if (t.state != State::Receiving)
        return;

    const auto end = decodeFileEnd(payload);
    if (!end || end->transferId != t.transferId)
        return;

    // **先**核对「实收 == 声明总长」。
    // FILE_END.bytesSent 只说「发送方认为它发了多少」——发送方在 HEADER 里声明
    // N+1、却只发 N 字节并把 bytesSent 与 sha 都按 N 字节算时，两者彼此自洽，
    // 但与本机记下的 t.totalSize 矛盾。少了这一条，就会静默落盘一个截断文件。
    if (t.received != t.totalSize) {
        conclude(from, false,
                 QStringLiteral("实收字节数与声明总长不符（声明 %1，实收 %2）")
                     .arg(t.totalSize)
                     .arg(t.received));
        return;
    }

    if (end->bytesSent != t.received) {
        conclude(from, false, QStringLiteral("字节数不符（声明 %1，实收 %2）")
                                  .arg(end->bytesSent)
                                  .arg(t.received));
        return;
    }

    if (end->sha256 != t.hash->result()) {
        conclude(from, false, QStringLiteral("sha256 校验失败"));
        return;
    }

    conclude(from, true, QStringLiteral("sha256 校验通过"));
}

void FileReceiver::abort(Source from)
{
    if (from) {
        auto it = transfers_.find(from);
        if (it != transfers_.end() && it->second && it->second->state == State::Receiving)
            conclude(from, false, QStringLiteral("传输中断，已丢弃未完成的文件"));
        if (lastActive_ == from)
            lastActive_ = nullptr;
        return;
    }

    // 无来源 = 清全部（停机路径）。先收集键，避免遍历中改容器。
    QVector<Source> keys;
    for (const auto &entry : transfers_) {
        if (entry.second && entry.second->state == State::Receiving)
            keys.append(entry.first);
    }
    for (Source s : keys)
        conclude(s, false, QStringLiteral("传输中断，已丢弃未完成的文件"));
}

void FileReceiver::conclude(Source from, bool ok, const QString &message)
{
    auto it = transfers_.find(from);
    if (it == transfers_.end() || !it->second)
        return;

    Transfer &t = *it->second;
    const QString path = t.targetPath;

    // 超时定时器必须先停：它一旦在 conclude 之后再响，就会去 conclude 一笔
    // 已经结束（甚至已被新一笔复用）的传输。
    if (t.timeout)
        t.timeout->stop();

    if (t.file) {
        t.file->close();
        t.file.reset();
    }
    t.hash.reset();
    t.state      = State::Idle;
    t.transferId = 0;
    t.totalSize  = 0;
    t.received   = 0;
    t.name.clear();
    t.targetPath.clear();

    if (lastActive_ == from)
        lastActive_ = nullptr;

    if (!ok && !path.isEmpty())
        QFile::remove(path);

    emit finished(ok, path, message);
}

} // namespace lanrdp
