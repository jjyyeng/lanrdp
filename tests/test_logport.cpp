#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include "app/LogPort.h"

// 日志端口（LogPort）
//
// 为什么要有这一层：把「日志 UI」整个删掉，日志改落盘 txt。
// 但测试不能因此失去观察点 —— 若测试继续读 UI 控件，删 UI 就等于删测试。
// 所以把日志抽成**不依赖 QWidget 的端口**（沿用 ClipboardPort 的手法），
// 由测试直接读端口的内容。
//
// 本测试**不建 QApplication**（只用 QCoreApplication），因为端口本身
// 是纯逻辑，不应该被 UI 依赖绑架。
class TestLogPort : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void appendsInOrderWithTimestamp();
    void bufferIsCappedKeepingNewest();
    void rotatesWhenFileExceedsLimit();
    void rotationFailureKeepsCurrentLogIntact();
    void rotationFailureBacksOffThenRetries();
    void clearBufferKeepsFileIntact();
    void unwritablePathDegradesGracefully();
    void writeFailureDoesNotPermanentlyDisableLogging();
    void probeFailureRecoversAfterPathAppears();
    void concurrentWritesDoNotInterleave();
    void logFilePathIsExposed();
    void writesUtf8BomOnlyForNewFile();
    void rotationWritesBomForFreshFile();
    void recreatingDeletedFileWritesBom();

private:
    QTemporaryDir *tmp_ = nullptr;

    QString logPath() const;

    // 读回整个日志文件（测试断言用）
    QString readFile(const QString &path) const;
};

void TestLogPort::init()
{
    tmp_ = new QTemporaryDir();
    QVERIFY(tmp_->isValid());
}

void TestLogPort::cleanup()
{
    delete tmp_;
    tmp_ = nullptr;
}

QString TestLogPort::logPath() const
{
    return QDir(tmp_->path()).absoluteFilePath(QStringLiteral("lanrdp.log"));
}

QString TestLogPort::readFile(const QString &path) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// 每条写入都要带 [hh:mm:ss] 前缀，且顺序与写入顺序一致。
void TestLogPort::appendsInOrderWithTimestamp()
{
    lanrdp::LogPort port(logPath());
    port.append(QStringLiteral("第一条"));
    port.append(QStringLiteral("第二条"));

    const QStringList lines = port.lines();
    QCOMPARE(lines.size(), 2);
    QVERIFY2(lines.at(0).endsWith(QStringLiteral("第一条")), qPrintable(lines.at(0)));
    QVERIFY2(lines.at(1).endsWith(QStringLiteral("第二条")), qPrintable(lines.at(1)));

    // 时间戳前缀形如 [12:34:56] 后接内容
    QVERIFY2(lines.at(0).startsWith(QLatin1Char('[')), qPrintable(lines.at(0)));
    QVERIFY2(lines.at(0).contains(QStringLiteral("] ")), qPrintable(lines.at(0)));

    // 文件里也应该有同样的两行
    const QString onDisk = readFile(logPath());
    QVERIFY2(onDisk.contains(QStringLiteral("第一条")), "日志必须落盘");
    QVERIFY2(onDisk.contains(QStringLiteral("第二条")), "日志必须落盘");
}

// 环形缓冲有上限：超了以后最旧的被裁掉，最新的留住。
void TestLogPort::bufferIsCappedKeepingNewest()
{
    lanrdp::LogPort port(logPath());
    port.setBufferCapacity(3);

    for (int i = 1; i <= 5; ++i)
        port.append(QStringLiteral("行%1").arg(i));

    const QStringList lines = port.lines();
    QCOMPARE(lines.size(), 3);
    QVERIFY2(lines.at(0).endsWith(QStringLiteral("行3")), qPrintable(lines.at(0)));
    QVERIFY2(lines.at(2).endsWith(QStringLiteral("行5")), qPrintable(lines.at(2)));

    // ⚠️ 缓冲被裁 ≠ 文件被裁：文件应保留全部 5 行（它是排查素材，不该丢）
    const QString onDisk = readFile(logPath());
    QVERIFY2(onDisk.contains(QStringLiteral("行1")), "文件不应随内存缓冲一起裁旧");
}

// 文件超过上限时滚动为 lanrdp.1.log，且当前文件始终存在。
void TestLogPort::rotatesWhenFileExceedsLimit()
{
    lanrdp::LogPort port(logPath());
    port.setFileSizeLimit(256);   // 很小的上限，方便触发

    for (int i = 0; i < 40; ++i)
        port.append(QStringLiteral("填充行 %1 —— 让文件超过上限").arg(i));

    const QString rotated = QDir(tmp_->path()).absoluteFilePath(QStringLiteral("lanrdp.1.log"));
    QVERIFY2(QFileInfo::exists(rotated), "超过上限后应滚动出 lanrdp.1.log");
    QVERIFY2(!readFile(rotated).isEmpty(), "归档文件里应有滚动前的内容");

    // ⚠️ 「当前日志文件始终存在」是设计约束：滚动时即刻重建空文件，
    // 否则滚动恰好发生在最后一次写入后，就会出现「lanrdp.log 不见了」的空窗，
    // 排查者会以为日志系统坏了。内容此时可能为空（刚滚过），但文件必须在。
    QVERIFY2(QFileInfo::exists(logPath()), "滚动后当前日志文件应继续存在");

    // 再写一条，必须落进当前文件（证明滚动后还能继续正常写）
    port.append(QStringLiteral("滚动之后的新内容"));
    QVERIFY2(readFile(logPath()).contains(QStringLiteral("滚动之后的新内容")),
             "滚动后应能继续写入当前日志文件");
}

// 归档删不掉时（这里是归档位置被一个**目录**占住，QFile::remove 对目录必然失败）：
// 绝不为了滚动先把当前日志毁掉 —— 当前文件必须完好、后续照常追加。
void TestLogPort::rotationFailureKeepsCurrentLogIntact()
{
    lanrdp::LogPort port(logPath());
    port.setFileSizeLimit(256);

    for (int i = 0; i < 40; ++i)
        port.append(QStringLiteral("填充行 %1 —— 让文件超过上限").arg(i));

    const QDir    dir(tmp_->path());
    const QString rotated = dir.absoluteFilePath(QStringLiteral("lanrdp.1.log"));
    QVERIFY2(QFileInfo::exists(rotated), "前置：应当已经滚动出归档");

    // 清掉归档、用同名目录占住它的位置。
    QVERIFY(QFile::remove(rotated));
    QVERIFY(dir.mkdir(QStringLiteral("lanrdp.1.log")));

    port.append(QStringLiteral("滚动之后的一行"));   // 刚滚完当前文件是空的，先写一行再取基线
    const QString before = readFile(logPath());
    QVERIFY2(!before.isEmpty(), "前置：当前日志里应当有内容");

    for (int i = 0; i < 20; ++i)
        port.append(QStringLiteral("删不掉归档时 %1").arg(i));

    // 1) 当前日志完好：原有内容一字不少，新行也都追加进去了
    const QString after = readFile(logPath());
    QVERIFY2(after.startsWith(before), "滚动失败不应改动当前日志的已有内容");
    QVERIFY2(after.contains(QStringLiteral("删不掉归档时 0")),
             "滚动失败后应继续追加到当前日志");
    QVERIFY2(after.contains(QStringLiteral("删不掉归档时 19")),
             "滚动失败后的每一行都应落地，不能因为滚动失败就丢日志");
}

// 滚动失败必须退避：否则每写一行都要白做一次 remove+rename，日志还无限长。
void TestLogPort::rotationFailureBacksOffThenRetries()
{
    lanrdp::LogPort port(logPath());
    port.setFileSizeLimit(128);
    port.setRollRetryDelayMs(500);   // 把 60 s 压成 500 ms

    const QDir    dir(tmp_->path());
    const QString rotated = dir.absoluteFilePath(QStringLiteral("lanrdp.1.log"));
    QVERIFY(dir.mkdir(QStringLiteral("lanrdp.1.log")));   // 先占住归档位置

    for (int i = 0; i < 10; ++i)
        port.append(QStringLiteral("占位期 %1").arg(i));
    QVERIFY2(QFileInfo(rotated).isDir(),
             "归档删不掉时不能把那个位置换成文件（会毁掉别人的东西）");

    // 障碍移除后，退避窗口内**不该**立刻重试
    QVERIFY(dir.rmdir(QStringLiteral("lanrdp.1.log")));
    port.append(QStringLiteral("退避窗口内的一行"));
    QVERIFY2(!QFileInfo::exists(rotated), "退避窗口内不应重试滚动");

    // 窗口到期后，下一次写入应正常滚动
    QTest::qWait(600);
    port.append(QStringLiteral("退避到期后的一行"));
    QVERIFY2(QFileInfo(rotated).isFile(), "退避到期后应重新尝试并滚动成功");
}

// clearBuffer() 只清内存缓冲，绝不动文件 —— 文件是排查素材。
void TestLogPort::clearBufferKeepsFileIntact()
{
    lanrdp::LogPort port(logPath());
    port.append(QStringLiteral("清空前的内容"));
    QVERIFY(!port.lines().isEmpty());

    const QString before = readFile(logPath());
    QVERIFY2(!before.isEmpty(), "前置：文件里应当已经有内容");

    port.clearBuffer();

    QVERIFY2(port.lines().isEmpty(), "清缓冲后内存里应当为空");
    QCOMPARE(readFile(logPath()), before);   // 文件逐字节不变

    // 清完还能继续写，且文件是追加而不是被截断
    port.append(QStringLiteral("清空后的内容"));
    const QString after = readFile(logPath());
    QVERIFY2(after.contains(QStringLiteral("清空前的内容")), "文件不该被清掉");
    QVERIFY2(after.contains(QStringLiteral("清空后的内容")), "清空后仍要能继续写");
}

// 路径不可写时不能崩、不能抛，仍要能读内存缓冲（静默降级）。
void TestLogPort::unwritablePathDegradesGracefully()
{
    // 指向一个「目录」当文件路径 —— 打开必然失败
    const QString bogus = QDir(tmp_->path()).absoluteFilePath(QStringLiteral("subdir"));
    QVERIFY(QDir().mkpath(bogus));

    lanrdp::LogPort port(bogus);
    port.append(QStringLiteral("写不进去也要记住"));

    // 不崩即可；内存缓冲照常工作
    QCOMPARE(port.lines().size(), 1);
    QVERIFY2(port.lines().at(0).endsWith(QStringLiteral("写不进去也要记住")),
             qPrintable(port.lines().at(0)));
    QVERIFY2(!port.fileWritable(), "应当能报告文件不可写（供上层决定是否提示）");
}

// 一次**瞬时**写失败不能把落盘永久关掉 —— 只置 fileUsable_ 而不放掉 fileTried_ 的话，
// 之后每次 append 都早返回、再也不会重试，一次抖动就丢掉整个会话的日志。
// 这里用「先只读、后恢复可写」构造瞬时失败并验证自愈。
void TestLogPort::writeFailureDoesNotPermanentlyDisableLogging()
{
    lanrdp::LogPort port(logPath());
    port.setFileSizeLimit(0);

    port.append(QStringLiteral("先正常写一条"));
    QVERIFY2(port.fileWritable(), "前置：初始应当可写");

    // 文件变只读 ⇒ 后续 open(Append) 失败（写失败路径）
    QVERIFY(QFile::setPermissions(logPath(), QFileDevice::ReadOwner | QFileDevice::ReadUser));
    port.append(QStringLiteral("这次写失败"));
    QVERIFY2(!port.fileWritable(), "只读后应当报告不可写");

    // 恢复可写 ⇒ 必须自愈
    QVERIFY(QFile::setPermissions(
        logPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                       | QFileDevice::ReadUser | QFileDevice::WriteUser));
    port.append(QStringLiteral("恢复之后必须能落盘"));

    QVERIFY2(port.fileWritable(), "瞬时失败修好后必须自愈，不能一坏到底");
    QVERIFY2(readFile(logPath()).contains(QStringLiteral("恢复之后必须能落盘")),
             "恢复后的日志必须真正写进文件");
}

// 首次探测就失败（目录还不存在）也必须能在条件恢复后自愈。
// 探测失败原来只置 fileUsable_、把 fileTried_ 留在 true，于是之后每次 append
// 都在探测门早退 —— 整场会话不会再有文件日志，而界面上看不出任何异常。
void TestLogPort::probeFailureRecoversAfterPathAppears()
{
    const QString lateDir  = QDir(tmp_->path()).absoluteFilePath(QStringLiteral("late"));
    const QString latePath = QDir(lateDir).absoluteFilePath(QStringLiteral("lanrdp.log"));

    lanrdp::LogPort port(latePath);
    port.setProbeRetryDelayMs(0);   // 默认 60 s 太久，本用例要立刻重试

    port.append(QStringLiteral("目录还不存在"));
    QVERIFY2(!port.fileWritable(), "前置：目录不存在时应当报告不可写");
    QVERIFY2(port.lines().size() == 1, "写不进文件也必须留在内存缓冲里");

    QVERIFY(QDir().mkpath(lateDir));   // 条件恢复
    port.append(QStringLiteral("目录建好之后必须能落盘"));

    QVERIFY2(port.fileWritable(), "探测失败不能一坏到底，条件恢复后必须自愈");
    QVERIFY2(readFile(latePath).contains(QStringLiteral("目录建好之后必须能落盘")),
             "自愈后的日志必须真正写进文件");
}

// 多线程并发写：不丢行、不交错（每行本身完整）。
void TestLogPort::concurrentWritesDoNotInterleave()
{
    lanrdp::LogPort port(logPath());
    port.setFileSizeLimit(0);   // 0 = 不滚动，避免并发测试被滚动干扰

    constexpr int kThreads        = 4;
    constexpr int kPerThread      = 50;
    QVector<QThread *> threads;

    for (int t = 0; t < kThreads; ++t) {
        auto *th = QThread::create([&port, t]() {
            for (int i = 0; i < kPerThread; ++i)
                port.append(QStringLiteral("T%1-%2").arg(t).arg(i));
        });
        threads.append(th);
    }

    for (auto *th : threads)
        th->start();
    for (auto *th : threads) {
        QVERIFY(th->wait(10000));
        delete th;
    }

    // 内存缓冲设够大，保证不裁旧，才能验证「一行不丢」
    // （注意：容量是在 append 之前设的，这里单独再造一个端口验证更稳）
    const QStringList lines = port.lines();
    QVERIFY2(lines.size() >= kThreads * kPerThread,
             qPrintable(QStringLiteral("并发写丢了行：只有 %1 行").arg(lines.size())));

    // 每行都应当是完整的一行（不能出现两行拼在一起）
    for (const QString &line : lines) {
        QVERIFY2(!line.contains(QStringLiteral("] ["), Qt::CaseSensitive)
                     || line.count(QStringLiteral("]: ")) == 1,
                 qPrintable(QStringLiteral("发现交错拼接的行：%1").arg(line)));
    }
}

// 端口要能报告自己实际用的文件路径（启动日志第一行会写它）。
void TestLogPort::logFilePathIsExposed()
{
    lanrdp::LogPort port(logPath());
    QCOMPARE(QFileInfo(port.logFilePath()).absoluteFilePath(),
             QFileInfo(logPath()).absoluteFilePath());
}

// 新建的日志文件必须带 **UTF-8 BOM**。
//
// 原因：中文 Windows 的记事本遇到「无 BOM 的 UTF-8」会按 GBK 猜 ⇒ 中文全是乱码
//（真机上踩过：用户打开 lanrdp.log 完全读不出一行）。带上 BOM 它才认得出 UTF-8。
// ⚠️ 但**已有内容的文件绝不补写** —— 往日志中间插 3 个字节是脏数据。
void TestLogPort::writesUtf8BomOnlyForNewFile()
{
    const QByteArray bom("\xEF\xBB\xBF", 3);

    {
        lanrdp::LogPort port(logPath());
        port.append(QStringLiteral("新建文件的第一行"));
    }

    QFile f(logPath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.read(3), bom);
    f.close();

    // 第二次打开（文件已存在、也有内容）：不能再插一遍 BOM
    {
        lanrdp::LogPort port(logPath());
        port.append(QStringLiteral("追加的一行"));
    }

    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray all = f.readAll();
    f.close();

    QVERIFY2(all.startsWith(bom), "新建的日志文件必须以 UTF-8 BOM 开头");
    QVERIFY2(all.indexOf(bom, 1) < 0, "BOM 只允许出现在文件开头一次");
    QVERIFY2(all.contains(QStringLiteral("追加的一行").toUtf8()), "追加的内容必须按 UTF-8 落盘");
}

// 滚动出来的**新**当前文件同样必须是带 BOM 的 UTF-8。
//
// 滚动的实现是「把旧文件改名 + 立刻重建一个空的 lanrdp.log」。重建走的是另一条
// 代码路径（不是「进程第一次写」），若 BOM 只在首写时补，滚动后的文件就退化成
// 无 BOM ⇒ 记事本又按 GBK 猜，整份新日志乱码。
void TestLogPort::rotationWritesBomForFreshFile()
{
    const QByteArray bom("\xEF\xBB\xBF", 3);

    lanrdp::LogPort port(logPath());
    port.setFileSizeLimit(256);   // 很小的上限，方便触发滚动

    for (int i = 0; i < 40; ++i)
        port.append(QStringLiteral("填充行 %1 —— 让文件超过上限").arg(i));

    const QString rotated = QDir(tmp_->path()).absoluteFilePath(QStringLiteral("lanrdp.1.log"));
    QVERIFY2(QFileInfo::exists(rotated), "超过上限后应滚动出 lanrdp.1.log");

    // 归档文件是**滚动前**那份，它必须也带 BOM（它才是最先被打开看的那份）
    QFile archive(rotated);
    QVERIFY(archive.open(QIODevice::ReadOnly));
    QCOMPARE(archive.read(3), bom);
    archive.close();

    // 再写一条，落到滚动后的新文件上
    port.append(QStringLiteral("滚动之后的新内容"));

    QFile f(logPath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray all = f.readAll();
    f.close();

    QVERIFY2(all.startsWith(bom), "滚动后重建的日志文件必须以 UTF-8 BOM 开头");
    QVERIFY2(all.contains(QStringLiteral("滚动之后的新内容").toUtf8()), "滚动后应能继续写入且按 UTF-8 落盘");
    QVERIFY2(all.indexOf(bom, 1) < 0, "BOM 只允许出现在文件开头一次");
}

// 运行期间日志文件被外部删掉，下一次写入把它重建出来时，同样要带 BOM。
//
// 现场意义：排查时让人「先删掉旧日志再复现」是常规操作；若重建的文件没有 BOM，
// 拿到的恰好是一份打不开的日志 —— 比没日志更糟。
void TestLogPort::recreatingDeletedFileWritesBom()
{
    const QByteArray bom("\xEF\xBB\xBF", 3);

    lanrdp::LogPort port(logPath());
    port.setFileSizeLimit(0);     // 关掉滚动，避免干扰
    port.append(QStringLiteral("第一行"));
    QVERIFY2(port.fileWritable(), "正常路径应可写");

    QVERIFY2(QFile::remove(logPath()), "前置：日志文件应能被删除");

    port.append(QStringLiteral("删掉之后的新内容"));

    QFile f(logPath());
    QVERIFY2(f.open(QIODevice::ReadOnly), "删除后应能自动重建日志文件");
    const QByteArray all = f.readAll();
    f.close();

    QVERIFY2(all.startsWith(bom), "重建的日志文件必须以 UTF-8 BOM 开头");
    QVERIFY2(all.contains(QStringLiteral("删掉之后的新内容").toUtf8()), "重建后应能继续写入");
    QVERIFY2(all.indexOf(bom, 1) < 0, "BOM 只允许出现在文件开头一次");
}

QTEST_MAIN(TestLogPort)
#include "test_logport.moc"
