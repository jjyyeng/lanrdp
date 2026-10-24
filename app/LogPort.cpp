#include "app/LogPort.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTextStream>

#include <cstdio>

namespace lanrdp {

namespace {

// UTF-8 BOM。中文 Windows 的记事本打开「无 BOM 的 UTF-8」会按 GBK 猜 ⇒ 中文全是乱码
//（真机上踩过：整份 lanrdp.log 读不出一行）。带上 BOM 它才认得出是 UTF-8。
// ⚠️ 只在**新建 / 滚动后重建**的那一刻写一次；已有内容的文件绝不补写。
const QByteArray kUtf8Bom("\xEF\xBB\xBF", 3);

} // namespace

LogPort::LogPort() = default;

LogPort::LogPort(const QString &filePath)
{
    // ⚠️ 必须绝对化：调用方（MainWindow / 测试）传进来的可能是相对路径，
    // 而 QFile 是按**进程当前工作目录**解析的 —— 程序被 Explorer 双击时，
    // 工作目录**不保证**是 exe 所在目录，日志就会散落到意外位置。
    // 统一在入口处绝对化，下游一切路径运算才有确定含义。
    filePath_ = filePath.isEmpty() ? QString()
                                   : QFileInfo(filePath).absoluteFilePath();
}

QString LogPort::stamped(const QString &line) const
{
    return QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")))
        .arg(line);
}

void LogPort::append(const QString &line)
{
    // 只在锁内做一次时间戳 —— 时间戳必须与「写入内存/文件」的内容完全一致。
    // （先前在锁外取时间戳、锁内再取一次，跨秒时两处不一致 → 测试抓到过。）
    QMutexLocker locker(&m_mutex_);
    const QString text = stamped(line);

    lines_.append(text);
    while (lines_.size() > bufferCapacity_)
        lines_.removeFirst();

    if (!filePath_.isEmpty())
        writeToFileLocked(text);
}

// 只在持锁时调用。
void LogPort::writeToFileLocked(const QString &text)
{
    // 首次写入才尝试打开 —— 避免每次 append 都做一次失败的文件系统探测。
    // 探测失败**不能就此一坏到底**（目录暂时不可用 / 文件被杀毒或备份程序短暂独占
    // 都会让首次打开失败），所以按退避窗口定期重试；窗口内只进内存缓冲，不碰磁盘。
    if (!fileTried_
        || (!fileUsable_ && QDateTime::currentMSecsSinceEpoch() >= probeRetryAtMs_)) {
        fileTried_  = true;
        fileUsable_ = false;

        QFile probe(filePath_);
        if (probe.open(QIODevice::Append | QIODevice::Text)) {
            probe.close();
            fileUsable_ = true;
        } else {
            probeRetryAtMs_ = QDateTime::currentMSecsSinceEpoch() + probeRetryDelayMs_;
        }
    }

    if (!fileUsable_)
        return;   // 静默降级：只进内存缓冲，不报错、不弹窗

    QFile file(filePath_);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        fileUsable_ = false;
        // 放掉「已探测过」的闩锁：写失败很可能是瞬时的（文件被短暂独占 / 磁盘忙）。
        // 不重置的话 fileTried_ 恒为 true，之后每次 append 都在上面那个探测门就早退、
        // **再也不会重试** —— 一次抖动就把整个会话的日志永久关掉。下次 append 重新探测即可自愈。
        fileTried_  = false;
        return;
    }

    // 以文件的**真实大小**为准，而不是沿用进程自己记的数：
    // 滚动会重建文件、用户可能在运行中把日志删掉或清空，两者都会让计数与磁盘不符。
    writtenBytes_ = file.size();

    // 空文件 = 这一份刚被建出来（首次写 / 滚动重建 / 被删除后重建）⇒ 先补 BOM。
    // 否则中文 Windows 的记事本会按 GBK 猜，整份日志乱码。
    // ⚠️ 有内容的文件绝不补写：往日志中间插 3 个字节是脏数据。
    if (writtenBytes_ == 0) {
        if (file.write(kUtf8Bom) == kUtf8Bom.size())
            writtenBytes_ = kUtf8Bom.size();
    }

    const QByteArray bytes = (text + QLatin1Char('\n')).toUtf8();
    if (file.write(bytes) != bytes.size()) {
        fileUsable_ = false;
        fileTried_  = false;   // 同上：允许下次 append 重新探测
        file.close();
        return;
    }

    // 每条都 flush：崩溃时最后几行才留得住，而那正是最有价值的部分。
    // 日志量是「连接事件级」的，这点开销可忽略。
    file.flush();
    file.close();

    writtenBytes_ += bytes.size();

    // 超上限则滚动：当前文件改名为 lanrdp.1.log（覆盖旧的归档）。
    //
    // ⚠️ 滚动必须**先把当前文件补回到 0 字节再往下走**，否则在「滚动后不再写」
    // 的情况下（比如测试刚好在滚动后停手）会留下一个「没有 lanrdp.log」的
    // 空窗 —— 下次 append 虽然会自动重建，但语义上「当前日志文件永远存在」
    // 更干净，也让排查者不必猜「日志怎么没了」。
    //
    // ⚠️ Windows 的 rename 不覆盖已存在的目标，所以只能「先删归档、再改名」，
    // 于是有两个失败面，必须分开处理：
    //   · 归档删不掉（被别的进程占着）⇒ **什么都不动**：当前日志完好、旧归档也还在。
    //     绝不为了滚动先毁掉一份还有内容的归档；
    //   · 改名失败（当前文件被占 / 权限 / 跨卷）⇒ 归档已删、当前文件未动，丢的只是
    //     上一份归档 —— 此时也立刻退避，别再每写一行重试一遍。
    // 退避窗口内不尝试滚动，日志继续追加到当前文件，下次窗口到期再试。
    if (fileSizeLimit_ > 0 && writtenBytes_ >= fileSizeLimit_
        && QDateTime::currentMSecsSinceEpoch() >= rollRetryAtMs_) {
        const QFileInfo info(filePath_);
        const QString rotated =
            info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
            + QStringLiteral(".1.") + info.suffix();

        bool rolled = false;
        if (!QFile::exists(rotated) || QFile::remove(rotated)) {
            if (QFile::rename(filePath_, rotated)) {
                // 立刻重建一个空的当前文件，保证「lanrdp.log 始终存在」
                QFile fresh(filePath_);
                if (fresh.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                    fresh.close();
                    writtenBytes_ = 0;
                    fileTried_    = true;
                    fileUsable_   = true;
                    rolled        = true;
                } else {
                    fileTried_ = false;   // 建不出来就下次重试探测
                }
            }
        }
        if (!rolled)
            rollRetryAtMs_ = QDateTime::currentMSecsSinceEpoch() + rollRetryDelayMs_;
    }
}

QStringList LogPort::lines() const
{
    QMutexLocker locker(&m_mutex_);
    return lines_;
}

void LogPort::clearBuffer()
{
    QMutexLocker locker(&m_mutex_);
    lines_.clear();
}

void LogPort::setBufferCapacity(int capacity)
{
    if (capacity <= 0)
        return;
    QMutexLocker locker(&m_mutex_);
    bufferCapacity_ = capacity;
    while (lines_.size() > bufferCapacity_)
        lines_.removeFirst();
}

int LogPort::bufferCapacity() const
{
    QMutexLocker locker(&m_mutex_);
    return bufferCapacity_;
}

void LogPort::setFileSizeLimit(qint64 bytes)
{
    QMutexLocker locker(&m_mutex_);
    fileSizeLimit_ = bytes;
    rollRetryAtMs_ = 0;   // 换了上限就不该继续等上一次失败的退避窗口
}

qint64 LogPort::fileSizeLimit() const
{
    QMutexLocker locker(&m_mutex_);
    return fileSizeLimit_;
}

QString LogPort::logFilePath() const
{
    QMutexLocker locker(&m_mutex_);
    return filePath_;
}

bool LogPort::fileWritable() const
{
    QMutexLocker locker(&m_mutex_);
    return fileUsable_;
}

} // namespace lanrdp
