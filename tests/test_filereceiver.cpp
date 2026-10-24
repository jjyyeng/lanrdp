#include <QtTest/QtTest>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QVector>

#include <limits>

#include "net/FileReceiver.h"
#include "protocol/FileTransfer.h"
#include "protocol/Frame.h"

// FileReceiver —— 消费 FILE_* 帧并落盘，收尾校验 sha256
class TestFileReceiver : public QObject
{
    Q_OBJECT

private slots:
    void receivesSingleChunkFile();
    void receivesMultiChunkFile();
    void rejectsChecksumMismatch();
    void rejectsByteCountMismatch();
    void rejectsTruncatedFileAgainstDeclaredSize();
    void sanitizesIncomingFileName();
    void rejectsDiscontinuousOffset();
    void abortRemovesPartialFile();
    void avoidsOverwritingExistingFile();
    void exhaustedCandidateNamesFailWithoutTruncating();
    void ignoresFramesOfUnknownTransfer();

    // 接收侧上限与剩余空间校验
    void rejectsFileOverMaxSize();
    void acceptsFileAtExactlyMaxSize();
    void rejectsWhenDiskSpaceInsufficient();

    // 多控制端并行传输的隔离
    void secondSessionDoesNotInterruptFirst();
    void abortOfOtherSessionKeepsTransferAlive();
    void sameTransferIdFromTwoSessionsStaysSeparate();
    void unknownSessionFrameIsIgnored();

    // 无数据块超时（对端发个 HEADER 就不动了）
    void stalledTransferTimesOutAndCleansUp();
    void timeoutDoesNotFireWhileDataKeepsArriving();
    void abortAfterTimeoutDoesNotEmitAgain();
};

namespace {

lanrdp::Frame makeHeaderFrame(quint32 id, const QString &name, quint64 total)
{
    lanrdp::Frame frame;
    frame.type    = static_cast<quint8>(lanrdp::FrameType::FileHeader);
    frame.payload = lanrdp::encodeFileHeader(lanrdp::FileHeader{id, name, total});
    return frame;
}

lanrdp::Frame makeChunkFrame(quint32 id, quint64 offset, const QByteArray &data)
{
    lanrdp::Frame frame;
    frame.type    = static_cast<quint8>(lanrdp::FrameType::FileChunk);
    frame.payload = lanrdp::encodeFileChunk(lanrdp::FileChunk{id, offset, data});
    return frame;
}

lanrdp::Frame makeEndFrame(quint32 id, quint64 bytesSent, const QByteArray &sha)
{
    lanrdp::Frame frame;
    frame.type    = static_cast<quint8>(lanrdp::FrameType::FileEnd);
    frame.payload = lanrdp::encodeFileEnd(lanrdp::FileEnd{id, bytesSent, sha});
    return frame;
}

// 按 64 KiB 分块，拼出完整的一次传输：HEADER + CHUNK* + END
QVector<lanrdp::Frame> buildTransfer(quint32           id,
                                     const QString    &name,
                                     const QByteArray &content,
                                     const QByteArray &shaOverride = QByteArray(),
                                     quint64           bytesOverride = 0)
{
    QVector<lanrdp::Frame> frames;
    frames.append(makeHeaderFrame(id, name, quint64(content.size())));

    for (int offset = 0; offset < content.size(); offset += lanrdp::kFileChunkSize)
        frames.append(makeChunkFrame(id, quint64(offset),
                                     content.mid(offset, lanrdp::kFileChunkSize)));

    const QByteArray sha = shaOverride.isEmpty() ? lanrdp::sha256Of(content) : shaOverride;
    const quint64    n   = bytesOverride != 0 ? bytesOverride : quint64(content.size());
    frames.append(makeEndFrame(id, n, sha));
    return frames;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

// 有确定性的伪随机内容（避免全 0 被压缩类逻辑掩盖问题）
QByteArray makeContent(int size)
{
    QByteArray data;
    data.resize(size);
    quint32 state = 0x12345678u;
    for (int i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        data[i] = static_cast<char>((state >> 16) & 0xFF);
    }
    return data;
}

struct Result
{
    int     count = 0;
    bool    ok    = false;
    QString path;
    QString message;
};

void capture(lanrdp::FileReceiver *receiver, Result *result)
{
    QObject::connect(receiver, &lanrdp::FileReceiver::finished, receiver,
                     [result](bool ok, const QString &path, const QString &message) {
                         ++result->count;
                         result->ok      = ok;
                         result->path    = path;
                         result->message = message;
                     });
}

// 传输来源在协议层是「哪条连接」；接收器只把它当不透明标识（从不解引用），
// 所以测试用几个固定地址即可，不必真的架 TCP。
lanrdp::FileReceiver::Source sourceA()
{
    static int a = 0;
    return &a;
}

lanrdp::FileReceiver::Source sourceB()
{
    static int b = 0;
    return &b;
}

} // namespace

void TestFileReceiver::receivesSingleChunkFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    const QByteArray content = QByteArrayLiteral("hello lanrdp\n");
    for (const lanrdp::Frame &frame : buildTransfer(1, QStringLiteral("hello.txt"), content))
        QVERIFY(receiver.onFrame(sourceA(), frame));

    QCOMPARE(result.count, 1);
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(QFileInfo(result.path).fileName(), QStringLiteral("hello.txt"));
    QCOMPARE(QFileInfo(result.path).absolutePath(),
             QFileInfo(dir.path()).absoluteFilePath());
    QCOMPARE(readAll(result.path), content);
}

void TestFileReceiver::receivesMultiChunkFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    // 3.5 块：跨多个 FILE_CHUNK，且最后一块不满
    const QByteArray content = makeContent(lanrdp::kFileChunkSize * 3 + 12345);
    for (const lanrdp::Frame &frame : buildTransfer(7, QStringLiteral("big.bin"), content))
        QVERIFY(receiver.onFrame(sourceA(), frame));

    QCOMPARE(result.count, 1);
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(readAll(result.path).size(), content.size());
    QCOMPARE(readAll(result.path), content);
    QCOMPARE(lanrdp::sha256Of(readAll(result.path)), lanrdp::sha256Of(content));
}

void TestFileReceiver::rejectsChecksumMismatch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    const QByteArray content = makeContent(4096);
    const QByteArray wrongSha(lanrdp::kSha256Size, '\x00');
    for (const lanrdp::Frame &frame :
         buildTransfer(2, QStringLiteral("bad.txt"), content, wrongSha))
        QVERIFY(receiver.onFrame(sourceA(), frame));

    QCOMPARE(result.count, 1);
    QVERIFY(!result.ok);
    // 校验失败不留残file
    QVERIFY(!QFile::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("bad.txt"))));
}

void TestFileReceiver::rejectsByteCountMismatch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    const QByteArray content = makeContent(1024);
    for (const lanrdp::Frame &frame :
         buildTransfer(3, QStringLiteral("n.txt"), content, QByteArray(), /*bytesOverride=*/999))
        QVERIFY(receiver.onFrame(sourceA(), frame));

    QCOMPARE(result.count, 1);
    QVERIFY(!result.ok);
    QVERIFY(!QFile::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("n.txt"))));
}

// `handleEnd` 校验了「声明的字节数 == 实收」，
// 又校验了 sha256，**唯独没校验「实收 == FILE_HEADER.totalSize」**。
//
// 于是出现这种组合时接收端会报「校验通过」：
// HEADER.totalSize = N+1 · 实发 N 字节 · FILE_END.bytesSent = N · sha = sha256(N 字节)
// —— 发送端两侧字段彼此自洽，却与「声明总长」矛盾。结果是**静默落盘一个截断文件**。
// 这既不要求攻击者伪造哈希，也不需要任何异常；纯逻辑就能构造。
void TestFileReceiver::rejectsTruncatedFileAgainstDeclaredSize()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    const QByteArray content = makeContent(2048);
    QVERIFY(receiver.onFrame(sourceA(), makeHeaderFrame(31, QStringLiteral("trunc.bin"),
                                             quint64(content.size()) + 1)));
    QVERIFY(receiver.onFrame(sourceA(), makeChunkFrame(31, 0, content)));
    QVERIFY(receiver.onFrame(sourceA(), makeEndFrame(31, quint64(content.size()),
                                          lanrdp::sha256Of(content))));

    QCOMPARE(result.count, 1);
    QVERIFY2(!result.ok,
             "实收字节数必须等于 FILE_HEADER 声明的总长；不等就是截断，不能判「校验通过」");
    QVERIFY2(!QFile::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("trunc.bin"))),
             "判失败时不该留下半截文件");
}

void TestFileReceiver::sanitizesIncomingFileName()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    const QByteArray content = QByteArrayLiteral("payload");
    // 对端试图目录穿越
    for (const lanrdp::Frame &frame :
         buildTransfer(4, QStringLiteral("../../evil.txt"), content))
        QVERIFY(receiver.onFrame(sourceA(), frame));

    QVERIFY(result.ok);
    const QFileInfo info(result.path);
    QCOMPARE(info.fileName(), QStringLiteral("evil.txt"));
    QCOMPARE(info.absolutePath(), QFileInfo(dir.path()).absoluteFilePath());

    // 确认没有写到上级目录
    QVERIFY(!QFile::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("../evil.txt"))));
}

void TestFileReceiver::rejectsDiscontinuousOffset()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    const QByteArray content = makeContent(2048);
    QVERIFY(receiver.onFrame(sourceA(), makeHeaderFrame(5, QStringLiteral("gap.bin"),
                                             quint64(content.size()))));
    // 故意从 offset=100 开始（中间缺 0..99），offset 字段就是用来抓这种错的
    QVERIFY(receiver.onFrame(sourceA(), makeChunkFrame(5, 100, content.mid(100))));
    QVERIFY(receiver.onFrame(sourceA(), makeEndFrame(5, quint64(content.size()), lanrdp::sha256Of(content))));

    QCOMPARE(result.count, 1);
    QVERIFY(!result.ok);
    QVERIFY(!QFile::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("gap.bin"))));
}

void TestFileReceiver::abortRemovesPartialFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    QVERIFY(receiver.onFrame(sourceA(), makeHeaderFrame(6, QStringLiteral("half.bin"), 100000)));
    QVERIFY(receiver.onFrame(sourceA(), makeChunkFrame(6, 0, makeContent(4096))));
    QVERIFY(receiver.busy());

    receiver.abort(sourceA());  // 模拟连接断开

    QCOMPARE(result.count, 1);
    QVERIFY(!result.ok);
    QVERIFY(!receiver.busy());
    QVERIFY(!QFile::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("half.bin"))));
}

void TestFileReceiver::avoidsOverwritingExistingFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 已存在同名文件，内容必须保住
    const QString existing = QDir(dir.path()).absoluteFilePath(QStringLiteral("a.txt"));
    {
        QFile file(existing);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("ORIGINAL");
    }

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    const QByteArray content = QByteArrayLiteral("new content");
    for (const lanrdp::Frame &frame : buildTransfer(8, QStringLiteral("a.txt"), content))
        QVERIFY(receiver.onFrame(sourceA(), frame));

    QVERIFY(result.ok);
    QCOMPARE(QFileInfo(result.path).fileName(), QStringLiteral("a-1.txt"));
    QCOMPARE(readAll(existing), QByteArrayLiteral("ORIGINAL"));
    QCOMPARE(readAll(result.path), content);
}

// 把「原名 + 全部 -N 候选」都占满后再发起传输。
// 接收端此时**没有可用文件名**，必须判失败并保留原有文件 ——
// 旧实现会回落到原名并用 Truncate 打开，把已有的 a.txt 清空后写入对端内容。
void TestFileReceiver::exhaustedCandidateNamesFailWithoutTruncating()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString original = QDir(dir.path()).absoluteFilePath(QStringLiteral("a.txt"));
    {
        QFile f(original);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.write("ORIGINAL") == 8);
    }
    // 造满 a-1.txt … a-9999.txt，让 uniquifyFileName 的候选名全部被占用
    for (int i = 1; i < 10000; ++i) {
        QFile f(QDir(dir.path()).absoluteFilePath(QStringLiteral("a-%1.txt").arg(i)));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    const QByteArray content = QByteArrayLiteral("new content");
    for (const lanrdp::Frame &frame : buildTransfer(51, QStringLiteral("a.txt"), content))
        QVERIFY(receiver.onFrame(sourceA(), frame));

    QCOMPARE(result.count, 1);
    QVERIFY2(!result.ok, "没有可用文件名时必须判失败，而不是去覆盖已有文件");
    QCOMPARE(readAll(original), QByteArrayLiteral("ORIGINAL"));
}

void TestFileReceiver::ignoresFramesOfUnknownTransfer()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    Result               result;
    capture(&receiver, &result);

    // 没有任何 HEADER 就先来 CHUNK / END：应被安静忽略，不产生 finished
    QVERIFY(receiver.onFrame(sourceA(), makeChunkFrame(99, 0, QByteArrayLiteral("junk"))));
    QVERIFY(receiver.onFrame(sourceA(), makeEndFrame(99, 4, QByteArray(lanrdp::kSha256Size, '\x01'))));

    QCOMPARE(result.count, 0);
    QVERIFY(!receiver.busy());

    // 非 FILE_* 类型应返回 false，表示「这帧不是给我的」
    lanrdp::Frame other;
    other.type = static_cast<quint8>(lanrdp::FrameType::InputEvent);
    QVERIFY(!receiver.onFrame(sourceA(), other));
}

// ---------------------------------------------------------------------------
// 接收侧上限与剩余空间校验
//
// 病灶：`handleHeader` 直接采信线上的 `quint64 totalSize` —— 既不查上限、也不查
// 剩余空间、也不问用户。一个已鉴权的对端就能一路把系统盘写满。
// 下面三条守卫「超限拒绝 / 边界放行 / 空间不足拒绝」。
// ---------------------------------------------------------------------------

// 声明尺寸超过上限 ⇒ 拒绝，且**根本不建文件**（不是收完再删）。
void TestFileReceiver::rejectsFileOverMaxSize()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    receiver.setMaxFileSize(1024);  // 1 KiB 上限

    Result result;
    capture(&receiver, &result);

    // 只发 HEADER（声明 2048 字节），应当在 HEADER 阶段就被拒。
    QVERIFY(receiver.onFrame(sourceA(), makeHeaderFrame(60, QStringLiteral("big.bin"), 2048)));

    QCOMPARE(result.count, 1);
    QVERIFY2(!result.ok, "超过上限的文件必须被拒绝");
    QVERIFY(!receiver.busy());
    QVERIFY2(!QFileInfo::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("big.bin"))),
             "拒绝时不该留下任何文件");
}

// 声明尺寸恰好等于上限 ⇒ 放行（边界不该误伤）。
void TestFileReceiver::acceptsFileAtExactlyMaxSize()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray content = makeContent(1000);

    lanrdp::FileReceiver receiver(dir.path());
    receiver.setMaxFileSize(quint64(content.size()));  // 上限 == 文件大小

    Result result;
    capture(&receiver, &result);

    for (const lanrdp::Frame &frame : buildTransfer(61, QStringLiteral("exact.bin"), content))
        QVERIFY(receiver.onFrame(sourceA(), frame));

    QCOMPARE(result.count, 1);
    QVERIFY2(result.ok, "恰好等于上限应放行");
    QCOMPARE(readAll(result.path), content);
}

// 剩余空间不足 ⇒ 拒绝（而不是收到一半才失败）。
void TestFileReceiver::rejectsWhenDiskSpaceInsufficient()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    // 上限设得极大（不再由上限拦），让「剩余空间」这一道成为唯一防线。
    receiver.setMaxFileSize(std::numeric_limits<quint64>::max());

    Result result;
    capture(&receiver, &result);

    // 声明一个远超任何真实磁盘的尺寸（16 EiB）。
    const quint64 absurd = std::numeric_limits<quint64>::max();
    QVERIFY(receiver.onFrame(sourceA(), makeHeaderFrame(62, QStringLiteral("huge.bin"), absurd)));

    QCOMPARE(result.count, 1);
    QVERIFY2(!result.ok, "剩余空间不足时必须拒绝");
    QVERIFY(!receiver.busy());
    QVERIFY2(!QFileInfo::exists(QDir(dir.path()).absoluteFilePath(QStringLiteral("huge.bin"))),
             "拒绝时不该留下任何文件");
}

// ---------------------------------------------------------------------------
// 多控制端并行传输的隔离。
//
// 病灶：`FileReceiver` 是**全进程单实例**，被控端所有连接共用一份状态。
// 于是「B 一发 HEADER」就能把 A 正在收的文件判失败并删掉（handleHeader 首行
// 无条件 conclude 掉进行中的传输）。下面四条守卫「按来源隔离」。
// ---------------------------------------------------------------------------

// B 发 HEADER 不得打断 A 正在进行的传输（两边各自完整收下）。
void TestFileReceiver::secondSessionDoesNotInterruptFirst()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());



    lanrdp::FileReceiver receiver(dir.path());

    QVector<QPair<bool, QString>> outcomes;
    QObject::connect(&receiver, &lanrdp::FileReceiver::finished, &receiver,
                     [&outcomes](bool ok, const QString &path, const QString &) {
                         outcomes.append({ok, path});
                     });

    const QByteArray contentA = makeContent(3000);
    const QByteArray contentB = makeContent(1500);
    const auto       framesA  = buildTransfer(1, QStringLiteral("a.bin"), contentA);
    const auto       framesB  = buildTransfer(2, QStringLiteral("b.bin"), contentB);

    // A 发 HEADER + 第一个 CHUNK（此时 A 的传输正进行到一半）
    QVERIFY(receiver.onFrame(sourceA(), framesA.at(0)));
    QVERIFY(receiver.onFrame(sourceA(), framesA.at(1)));

    // B 插进来发自己的 HEADER —— A 不该因此被判失败
    QVERIFY(receiver.onFrame(sourceB(), framesB.at(0)));

    // A 继续发完
    for (int i = 2; i < framesA.size(); ++i)
        QVERIFY(receiver.onFrame(sourceA(), framesA.at(i)));
    // B 也发完
    for (int i = 1; i < framesB.size(); ++i)
        QVERIFY(receiver.onFrame(sourceB(), framesB.at(i)));

    QCOMPARE(outcomes.size(), 2);
    for (const auto &o : outcomes)
        QVERIFY2(o.first, "两路都该成功收完");

    // 两份内容都完整落盘且不串味
    QVector<QByteArray> got;
    for (const auto &o : outcomes)
        got.append(readAll(o.second));
    QVERIFY(got.contains(contentA));
    QVERIFY(got.contains(contentB));
}

// abort(某个来源) 只能清掉那一份，不得连累别人。
void TestFileReceiver::abortOfOtherSessionKeepsTransferAlive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());



    lanrdp::FileReceiver receiver(dir.path());

    QVector<bool> outcomes;
    QObject::connect(&receiver, &lanrdp::FileReceiver::finished, &receiver,
                     [&outcomes](bool ok, const QString &, const QString &) {
                         outcomes.append(ok);
                     });

    const QByteArray contentA = makeContent(2000);
    const auto       framesA  = buildTransfer(1, QStringLiteral("a.bin"), contentA);
    const auto       framesB  = buildTransfer(2, QStringLiteral("b.bin"), makeContent(500));

    // 两路都开起来
    QVERIFY(receiver.onFrame(sourceA(), framesA.at(0)));
    QVERIFY(receiver.onFrame(sourceB(), framesB.at(0)));
    QVERIFY(receiver.busy(sourceA()));
    QVERIFY(receiver.busy(sourceB()));

    // B 掉线 —— A 必须还活着
    receiver.abort(sourceB());
    QVERIFY2(receiver.busy(sourceA()), "别人的断连不该清掉我的传输");
    QVERIFY(!receiver.busy(sourceB()));

    // B 的失败结算恰好一次（abort 也算一次结论）
    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes.first());

    // A 继续发完，仍应成功
    for (int i = 1; i < framesA.size(); ++i)
        QVERIFY(receiver.onFrame(sourceA(), framesA.at(i)));
    QCOMPARE(outcomes.size(), 2);
    QVERIFY2(outcomes.last(), "A 不该被 B 的断连影响");
    QCOMPARE(readAll(QDir(dir.path()).absoluteFilePath(QStringLiteral("a.bin"))),
             contentA);
}

// 两条连接用**同一个 transferId** 同时传**不同内容**，不得串味。
void TestFileReceiver::sameTransferIdFromTwoSessionsStaysSeparate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());



    lanrdp::FileReceiver receiver(dir.path());

    QVector<QPair<bool, QString>> outcomes;
    QObject::connect(&receiver, &lanrdp::FileReceiver::finished, &receiver,
                     [&outcomes](bool ok, const QString &path, const QString &) {
                         outcomes.append({ok, path});
                     });

    // 同 id、不同内容 —— 交错推进
    const QByteArray contentA = makeContent(1200);
    const QByteArray contentB = makeContent(800);
    const auto       framesA  = buildTransfer(7, QStringLiteral("a.bin"), contentA);
    const auto       framesB  = buildTransfer(7, QStringLiteral("b.bin"), contentB);

    QVERIFY(receiver.onFrame(sourceA(), framesA.at(0)));
    QVERIFY(receiver.onFrame(sourceB(), framesB.at(0)));

    const int maxN = qMax(framesA.size(), framesB.size());
    for (int i = 1; i < maxN; ++i) {
        if (i < framesA.size())
            QVERIFY(receiver.onFrame(sourceA(), framesA.at(i)));
        if (i < framesB.size())
            QVERIFY(receiver.onFrame(sourceB(), framesB.at(i)));
    }

    QCOMPARE(outcomes.size(), 2);
    for (const auto &o : outcomes)
        QVERIFY(o.first);

    QVector<QByteArray> got;
    for (const auto &o : outcomes)
        got.append(readAll(o.second));
    QVERIFY2(got.contains(contentA), "A 的内容必须完整（同 id 不该被 B 的块顶掉）");
    QVERIFY2(got.contains(contentB), "B 的内容必须完整");
}

// 从未见过的来源发帧 ⇒ 不崩、也不得误伤在跑的传输。
void TestFileReceiver::unknownSessionFrameIsIgnored()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());



    lanrdp::FileReceiver receiver(dir.path());

    QVector<bool> outcomes;
    QObject::connect(&receiver, &lanrdp::FileReceiver::finished, &receiver,
                     [&outcomes](bool ok, const QString &, const QString &) {
                         outcomes.append(ok);
                     });

    const QByteArray contentA = makeContent(1000);
    const auto       framesA  = buildTransfer(1, QStringLiteral("a.bin"), contentA);

    QVERIFY(receiver.onFrame(sourceA(), framesA.at(0)));

    // 一个不属于任何连接来源的帧（传 nullptr）
    QVERIFY(receiver.onFrame(nullptr, makeChunkFrame(1, 0, QByteArrayLiteral("junk"))));
    QVERIFY(receiver.onFrame(nullptr, makeEndFrame(1, 0, QByteArray())));

    // A 仍完好
    QVERIFY(receiver.busy(sourceA()));
    QVERIFY(outcomes.isEmpty());

    for (int i = 1; i < framesA.size(); ++i)
        QVERIFY(receiver.onFrame(sourceA(), framesA.at(i)));

    QCOMPARE(outcomes.size(), 1);
    QVERIFY(outcomes.first());
    QCOMPARE(readAll(QDir(dir.path()).absoluteFilePath(QStringLiteral("a.bin"))),
             contentA);
}

// 对端发来 HEADER 之后就再也不发块（却照常回心跳 —— 死链判定把任何字节都当活性）。
// 没有超时的话：文件句柄被长期占住、磁盘上留一个 0 字节目标文件、UI 停在「接收中」，
// 而**下一次正常传输还会静默 abort 它**（同来源的新 HEADER 把旧的判成「被打断」）。
void TestFileReceiver::stalledTransferTimesOutAndCleansUp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    receiver.setTransferTimeoutMs(200);   // 缩短，别拖慢用例

    Result result;
    capture(&receiver, &result);

    lanrdp::FileReceiver::Source from = sourceA();
    QVERIFY(receiver.onFrame(from, makeHeaderFrame(7, QStringLiteral("stalled.bin"), 4096)));

    QVERIFY2(receiver.busy(from), "刚开文件时应当在接收中");
    QCOMPARE(receiver.activeCount(), 1);
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1);   // 文件此刻已建出

    // 一个块都不喂 —— 等它自己超时
    QTRY_VERIFY_WITH_TIMEOUT(result.count == 1, 5000);
    QVERIFY2(!result.ok, "超时必须判失败");
    QVERIFY2(result.message.contains(QStringLiteral("超时")), qPrintable(result.message));

    // 半截文件删掉、句柄释放、UI 不再停在「接收中」
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);
    QVERIFY2(!receiver.busy(), "超时后不该还显示在接收中");
    QCOMPARE(receiver.activeCount(), 0);
}

// 计时口径是「**无块**」而不是「总时长」：慢慢传（但一直在传）不该被误杀。
void TestFileReceiver::timeoutDoesNotFireWhileDataKeepsArriving()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    receiver.setTransferTimeoutMs(400);

    Result result;
    capture(&receiver, &result);

    lanrdp::FileReceiver::Source from = sourceA();
    // 3 块：帧间隔 150 ms（< 400 ms），但整笔累计远超 400 ms
    const QByteArray content = makeContent(int(lanrdp::kFileChunkSize) * 3);
    const auto       frames  = buildTransfer(11, QStringLiteral("slow.bin"), content);

    for (const lanrdp::Frame &f : frames) {
        QVERIFY(receiver.onFrame(from, f));
        QTest::qWait(150);
    }

    QCOMPARE(result.count, 1);
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(QFileInfo(result.path).fileName(), QStringLiteral("slow.bin"));
    QCOMPARE(readAll(result.path), content);
}

// 超时之后还会有别的路径来收尾（断连 abort / 停机 abort）：conclude 必须幂等，
// 不得再 emit 一次 finished，且同一来源之后还能正常再传一笔。
void TestFileReceiver::abortAfterTimeoutDoesNotEmitAgain()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    lanrdp::FileReceiver receiver(dir.path());
    receiver.setTransferTimeoutMs(150);

    Result result;
    capture(&receiver, &result);

    lanrdp::FileReceiver::Source from = sourceA();
    QVERIFY(receiver.onFrame(from, makeHeaderFrame(3, QStringLiteral("stall.bin"), 1024)));
    QTRY_VERIFY_WITH_TIMEOUT(result.count == 1, 5000);
    QVERIFY(!result.ok);

    receiver.abort(from);   // 断连路径
    receiver.abort();       // 停机路径（清全部）
    QTest::qWait(250);
    QCOMPARE(result.count, 1);   // 不得重复 emit

    // 同一来源再来一笔正常的：必须成功（超时没有留下残状态）
    const QByteArray content = makeContent(4096);
    for (const lanrdp::Frame &f : buildTransfer(4, QStringLiteral("after.bin"), content))
        QVERIFY(receiver.onFrame(from, f));

    QCOMPARE(result.count, 2);
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(QFileInfo(result.path).fileName(), QStringLiteral("after.bin"));
}

QTEST_GUILESS_MAIN(TestFileReceiver)
#include "test_filereceiver.moc"
