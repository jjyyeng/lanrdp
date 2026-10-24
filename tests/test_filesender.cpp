#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QVector>

#include <memory>

#include "net/FileSender.h"
#include "net/Session.h"
#include "protocol/FileTransfer.h"
#include "protocol/Frame.h"

// FileSender —— 读文件、分块、流式 sha256、背压
class TestFileSender : public QObject
{
    Q_OBJECT

private slots:
    void reportsUnreadableFile();
    void rejectsEmptyFile();
    void streamsFileInOrderedChunks();
    void sendsTwoTransfersInSequence();
    void abortMarksTransferFailed();
    // 源文件在传输中途被截断/读取出错
    void reportsTruncatedFileMidTransfer();
};

namespace {

QByteArray makeContent(int size)
{
    QByteArray data;
    data.resize(size);
    quint32 state = 0x9E3779B9u;
    for (int i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        data[i] = static_cast<char>((state >> 13) & 0xFF);
    }
    return data;
}

QString writeTempFile(const QTemporaryDir &dir, const QString &name, const QByteArray &content)
{
    const QString path = QDir(dir.path()).absoluteFilePath(name);
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    file.close();
    return path;
}

// loopback 上的一对 Session：指定哪一侧当发送方，另一侧收集帧
struct Wire
{
    QTcpServer                      server;
    std::unique_ptr<lanrdp::Session> senderSide;
    std::unique_ptr<lanrdp::Session> peerSide;
    QVector<lanrdp::Frame>           frames;
};

bool setupWire(Wire *wire)
{
    if (!wire->server.listen(QHostAddress::LocalHost, 0))
        return false;

    auto *clientSocket = new QTcpSocket();
    clientSocket->connectToHost(QHostAddress::LocalHost, wire->server.serverPort());
    if (!clientSocket->waitForConnected(3000))
        return false;
    if (!wire->server.waitForNewConnection(3000))
        return false;

    QTcpSocket *serverSide = wire->server.nextPendingConnection();
    if (!serverSide)
        return false;

    // Session 接管 socket 所有权（setParent），因此两端都用堆对象
    wire->senderSide = std::make_unique<lanrdp::Session>(clientSocket);
    wire->peerSide   = std::make_unique<lanrdp::Session>(serverSide);

    QObject::connect(wire->peerSide.get(), &lanrdp::Session::frameReceived,
                     wire->peerSide.get(),
                     [wire](const lanrdp::Frame &frame) { wire->frames.append(frame); });
    return true;
}

struct RunResult
{
    int     count = 0;
    bool    ok    = false;
    QString path;
    QString message;
};

void watch(lanrdp::FileSender *sender, RunResult *result)
{
    QObject::connect(sender, &lanrdp::FileSender::finished, sender,
                     [result](bool ok, const QString &path, const QString &message) {
                         ++result->count;
                         result->ok      = ok;
                         result->path    = path;
                         result->message = message;
                     });
}

} // namespace

void TestFileSender::reportsUnreadableFile()
{
    lanrdp::FileSender sender(nullptr);
    RunResult          result;
    watch(&sender, &result);

    QVERIFY(!sender.start(QDir::temp().absoluteFilePath(QStringLiteral("lanrdp-no-such-file.bin")), 1));
    QCOMPARE(result.count, 1);
    QVERIFY(!result.ok);
    QVERIFY(!result.message.isEmpty());
}

void TestFileSender::rejectsEmptyFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeTempFile(dir, QStringLiteral("empty.bin"), QByteArray());
    QVERIFY(!path.isEmpty());

    lanrdp::FileSender sender(nullptr);
    RunResult          result;
    watch(&sender, &result);

    QVERIFY(!sender.start(path, 2));
    QCOMPARE(result.count, 1);
    QVERIFY(!result.ok);
}

void TestFileSender::streamsFileInOrderedChunks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 跨 3 块，最后一块不满
    const QByteArray content = makeContent(lanrdp::kFileChunkSize * 2 + 777);
    const QString    path    = writeTempFile(dir, QStringLiteral("payload.bin"), content);
    QVERIFY(!path.isEmpty());

    Wire wire;
    QVERIFY(setupWire(&wire));

    lanrdp::FileSender sender(wire.senderSide.get());
    RunResult          result;
    watch(&sender, &result);

    QVERIFY(sender.start(path, 11));
    QCOMPARE(sender.fileName(), QStringLiteral("payload.bin"));
    QCOMPARE(sender.totalBytes(), quint64(content.size()));

    QTRY_COMPARE_WITH_TIMEOUT(result.count, 1, 5000);
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.path, path);

    // 等对端把帧收齐：HEADER + 3 CHUNK + END
    QTRY_COMPARE_WITH_TIMEOUT(wire.frames.size(), 5, 5000);

    // 首帧 HEADER
    const auto header = lanrdp::decodeFileHeader(wire.frames.at(0).payload);
    QVERIFY(header.has_value());
    QVERIFY(wire.frames.at(0).type == static_cast<quint8>(lanrdp::FrameType::FileHeader));
    QCOMPARE(header->transferId, 11u);
    QCOMPARE(header->name, QStringLiteral("payload.bin"));
    QCOMPARE(header->totalSize, quint64(content.size()));

    // 中间是 CHUNK，偏移必须严格连续
    QByteArray reassembled;
    quint64    expectedOffset = 0;
    for (int i = 1; i <= 3; ++i) {
        QVERIFY(wire.frames.at(i).type == static_cast<quint8>(lanrdp::FrameType::FileChunk));
        const auto chunk = lanrdp::decodeFileChunk(wire.frames.at(i).payload);
        QVERIFY(chunk.has_value());
        QCOMPARE(chunk->transferId, 11u);
        QCOMPARE(chunk->offset, expectedOffset);
        QVERIFY(chunk->data.size() <= lanrdp::kFileChunkSize);
        reassembled.append(chunk->data);
        expectedOffset += quint64(chunk->data.size());
    }
    QCOMPARE(reassembled, content);

    // 末帧 END：字节数与 sha256 都要对
    QVERIFY(wire.frames.at(4).type == static_cast<quint8>(lanrdp::FrameType::FileEnd));
    const auto end = lanrdp::decodeFileEnd(wire.frames.at(4).payload);
    QVERIFY(end.has_value());
    QCOMPARE(end->transferId, 11u);
    QCOMPARE(end->bytesSent, quint64(content.size()));
    QCOMPARE(end->sha256, lanrdp::sha256Of(content));
    QCOMPARE(sender.sha256(), lanrdp::sha256Of(content));

    QVERIFY(!sender.busy());
}

void TestFileSender::sendsTwoTransfersInSequence()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray first  = makeContent(lanrdp::kFileChunkSize + 5);
    const QByteArray second = makeContent(4096);
    const QString    pathA  = writeTempFile(dir, QStringLiteral("a.bin"), first);
    const QString    pathB  = writeTempFile(dir, QStringLiteral("b.bin"), second);
    QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

    Wire wire;
    QVERIFY(setupWire(&wire));

    lanrdp::FileSender sender(wire.senderSide.get());
    RunResult          result;
    watch(&sender, &result);

    QVERIFY(sender.start(pathA, 21));
    QTRY_COMPARE_WITH_TIMEOUT(result.count, 1, 5000);
    QVERIFY(result.ok);
    // 发送方可能在 start() 内同步就发完了，对端收齐是异步的，必须等
    QTRY_VERIFY_WITH_TIMEOUT(wire.frames.size() >= 3, 5000);
    const int afterFirst = wire.frames.size();

    QVERIFY(sender.start(pathB, 22));
    QTRY_COMPARE_WITH_TIMEOUT(result.count, 2, 5000);
    QVERIFY(result.ok);

    QTRY_VERIFY_WITH_TIMEOUT(wire.frames.size() > afterFirst + 2, 5000);

    // 第二笔的 HEADER 必须带新的 transferId，且内容是第一笔之后的
    const auto headerB = lanrdp::decodeFileHeader(wire.frames.at(afterFirst).payload);
    QVERIFY(headerB.has_value());
    QCOMPARE(headerB->transferId, 22u);
    QCOMPARE(headerB->name, QStringLiteral("b.bin"));

    const auto endB = lanrdp::decodeFileEnd(wire.frames.last().payload);
    QVERIFY(endB.has_value());
    QCOMPARE(endB->transferId, 22u);
    QCOMPARE(endB->sha256, lanrdp::sha256Of(second));
}

void TestFileSender::abortMarksTransferFailed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 必须大于单次 pump 预算（4 MiB），否则 start() 内就把整个文件发完了，
    // 也就没有「传输进行中」这个状态可中止
    const QByteArray content = makeContent(6 * 1024 * 1024 + 123);
    const QString    path    = writeTempFile(dir, QStringLiteral("c.bin"), content);
    QVERIFY(!path.isEmpty());

    Wire wire;
    QVERIFY(setupWire(&wire));

    lanrdp::FileSender sender(wire.senderSide.get());
    RunResult          result;
    watch(&sender, &result);

    QVERIFY(sender.start(path, 41));
    QVERIFY(sender.busy());

    sender.abort();

    QCOMPARE(result.count, 1);
    QVERIFY(!result.ok);
    QVERIFY(!sender.busy());
}

// `pump` 把「读到空」一律当成「文件读完」。
// 于是源文件在传输中途被改小 / 读取出错时，发送端会照常发出 FILE_END 并报
// 「发送完成」—— 而它实际只发了一半。接收端新加的 received_==totalSize_ 能兜住，
// 但那是**对方**在替我们发现；发送端自己必须先失败。
void TestFileSender::reportsTruncatedFileMidTransfer()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 必须大于单次 pump 预算（4 MiB），否则 start() 内就发完了，没有「传输中」可截断
    const QByteArray content = makeContent(6 * 1024 * 1024 + 321);
    const QString    path    = writeTempFile(dir, QStringLiteral("d.bin"), content);
    QVERIFY(!path.isEmpty());

    Wire wire;
    QVERIFY(setupWire(&wire));

    lanrdp::FileSender sender(wire.senderSide.get());
    RunResult          result;
    watch(&sender, &result);

    QVERIFY(sender.start(path, 61));
    QVERIFY(sender.busy());   // 确实还在传

    // 用第二个句柄把文件截断：模拟「源文件在传输中被改小」（另一进程改写 / 设备错误）。
    {
        QFile truncator(path);
        QVERIFY2(truncator.open(QIODevice::ReadWrite), "需要第二个句柄来模拟中途截断");
        QVERIFY(truncator.resize(0));
    }

    QTRY_COMPARE_WITH_TIMEOUT(result.count, 1, 10000);
    QVERIFY2(!result.ok,
             "源文件在传输中变了就必须报失败，不能照常发 FILE_END 说「发送完成」");
    QVERIFY(!sender.busy());
}

QTEST_GUILESS_MAIN(TestFileSender)
#include "test_filesender.moc"
