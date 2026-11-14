#pragma once

#include <QMutex>
#include <QString>
#include <QStringList>

namespace lanrdp {

// 日志端口。
//
// 为什么单独抽一层：
// 把界面上的日志区**整个删掉**（用户要求），日志改落盘 txt 供事后排查。
// 若日志只写 UI 控件，删掉 UI 就等于删掉测试的观察点；所以把日志做成
// 不依赖 QWidget 的**可注入端口**（与 ClipboardPort 同一手法）：
// · 内存环形缓冲 —— 供程序内部与测试读取（UI 想加回来也随时可以）；
// · 追加落盘 —— 供事后排查（用户明确选 dist/lanrdp/lanrdp.log）；
// · 控制台 stdout —— 保留既有 CLI 自测用法。
//
// 线程安全：内部用 QMutex 保护。现在日志基本只在主线程写，但一旦将来引入
// 第二个线程写日志，无互斥就是 UB —— 提前堵住，成本近乎零。
//
// ⚠️ 落盘失败必须**静默降级**（只进内存缓冲）：日志系统不该拖垮主程序。
class LogPort
{
public:
    // 默认上限：环形缓冲 1000 行、单文件 5 MB（超了转 lanrdp.1.log）。
    static constexpr int   kDefaultBufferCapacity = 1000;
    static constexpr qint64 kDefaultFileSizeLimit = 5 * 1024 * 1024;

    // 不写文件（只进内存缓冲 + 控制台）。给测试与「无落盘」场景用。
    LogPort();
    explicit LogPort(const QString &filePath);

    // 追加一条日志（自动加 [hh:mm:ss] 前缀）。不写日志头。
    void append(const QString &line);

    // 内存缓冲的当前内容（按写入顺序，已被环形裁剪）。
    QStringList lines() const;

    // 只清**内存缓冲**，绝不动文件 —— 文件是排查素材，
    // 不能被一个界面按钮清掉，它只受尺寸滚动控制。
    void clearBuffer();

    // 环形缓冲容量（行）。
    void setBufferCapacity(int capacity);
    int  bufferCapacity() const;

    // 单文件尺寸上限（字节）。传 0 表示**不滚动**（无限增长）。
    void setFileSizeLimit(qint64 bytes);
    qint64 fileSizeLimit() const;

    // 滚动失败后的退避间隔（毫秒，默认 60 s）。
    // 失败时不退避就会**每写一行重试一遍** remove+rename —— 两次系统调用白做，
    // 日志还会无限增长。给测试把 60 s 压成毫秒用。
    void setRollRetryDelayMs(qint64 ms) { rollRetryDelayMs_ = ms; }
    qint64 rollRetryDelayMs() const { return rollRetryDelayMs_; }

    // 首次探测失败后的重试间隔（毫秒，默认 60 s）。
    // 与滚动退避同一个理由：探测失败若就此放弃，一次抖动就把整场会话的日志关掉；
    // 但也不能每写一行重试一遍（那是一次多余的文件系统探测）。
    void setProbeRetryDelayMs(qint64 ms) { probeRetryDelayMs_ = ms; }
    qint64 probeRetryDelayMs() const { return probeRetryDelayMs_; }

    // 实际使用的日志文件路径（空 = 不落盘）。启动日志第一行会写它。
    QString logFilePath() const;

    // 文件当前是否可写。false 表示已降级为「仅内存 + 控制台」。
    bool fileWritable() const;

private:
    void    writeToFileLocked(const QString &line);
    QString stamped(const QString &line) const;

    mutable QMutex m_mutex_;
    QStringList    lines_;
    int            bufferCapacity_ = kDefaultBufferCapacity;
    QString        filePath_;
    qint64         fileSizeLimit_ = kDefaultFileSizeLimit;
    qint64         writtenBytes_  = 0;   // 当前文件已写字节（避免每次 stat）
    bool           fileUsable_    = false;  // 路径可用且已成功写过
    bool           fileTried_     = false;  // 是否已尝试过打开

    // 滚动失败后的退避：早于该时刻（QDateTime 毫秒）不再尝试滚动。
    qint64         rollRetryDelayMs_ = 60 * 1000;
    qint64         rollRetryAtMs_    = 0;

    // 首次探测失败后的退避：早于该时刻不再探测（fileTried_ 仍为 true）。
    qint64         probeRetryDelayMs_ = 60 * 1000;
    qint64         probeRetryAtMs_    = 0;
};

} // namespace lanrdp
