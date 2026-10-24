#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <memory>

#include "net/FileReceiver.h"
#include "net/FileSender.h"
#include "net/Session.h"
#include "protocol/FileTransfer.h"
#include "protocol/Frame.h"

// 端到端 —— FileSender 经真实 loopback socket 送到 FileReceiver 落盘
// 验收标准：接收文件与源文件 sha256 一致。
class TestFileTransferE2E : public QObject
{
    Q_OBJECT

private slots:
    void transfersFileAcrossMultipleChunks();
    void transfersLargeFileWithBackpressure();
    void transfersTwoFilesBackToBack();
    void twoConnectionsTransferConcurrently();
    // 中途断链：不得把半截文件当成品留在磁盘上，也不得判成功
    void midTransferDisconnectLeavesNoPartialFile();
};

namespace {

QByteArray makeContent(int size)
{
    QByteArray data;
    data.resize(size);
    quint32 state = 0xC0FFEEu;
    for (int i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        data[i] = static_cast<char>((state >> 11) & 0xFF);
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

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

struct Result
{
    int     count = 0;
    bool    ok    = false;
    QString path;
    QString message;
};

// loopback 上的一对 Session + 挂在接收侧的 FileReceiver
struct Pair
{
    QTcpServer                            server;
    std::unique_ptr<lanrdp::Session>      senderSide;
    std::unique_ptr<lanrdp::Session>      receiverSide;
    std::unique_ptr<lanrdp::FileReceiver> receiver;
    QTcpSocket                           *clientSocket = nullptr;   // 断链用例用
    Result                                result;
};

bool setup(Pair *pair, const QString &downloadDir)
{
    if (!pair->server.listen(QHostAddress::LocalHost, 0))
        return false;

    auto *clientSocket = new QTcpSocket();
    clientSocket->connectToHost(QHostAddress::LocalHost, pair->server.serverPort());
    if (!clientSocket->waitForConnected(3000))
        return false;
    if (!pair->server.waitForNewConnection(3000))
        return false;
    pair->clientSocket = clientSocket;   // 断链用例要能掐它

    QTcpSocket *serverSide = pair->server.nextPendingConnection();
    if (!serverSide)
        return false;

    pair->senderSide   = std::make_unique<lanrdp::Session>(clientSocket);
    pair->receiverSide = std::make_unique<lanrdp::Session>(serverSide);
    pair->receiver     = std::make_unique<lanrdp::FileReceiver>(downloadDir);

    // 接收侧 Session 的每一帧都交给 FileReceiver（来源 = 这条连接的接收侧 Session）
    QObject::connect(pair->receiverSide.get(), &lanrdp::Session::frameReceived,
                     pair->receiver.get(),
                     [pair](const lanrdp::Frame &frame) {
                         pair->receiver->onFrame(pair->receiverSide.get(), frame);
                     });

    // 链路死掉 = 这一路传输作废，半截文件必须丢掉。
    // 这正是生产里的接线（控制端掉线 / 网线被拔），从前没有任何用例走过它 ——
    // 半截文件清理只靠直接调 abort() 覆盖，等于没测过真正的断开路径。
    QObject::connect(pair->receiverSide.get(), &lanrdp::Session::disconnected,
                     pair->receiver.get(),
                     [pair]() { pair->receiver->abort(pair->receiverSide.get()); });

    QObject::connect(pair->receiver.get(), &lanrdp::FileReceiver::finished,
                     pair->receiver.get(),
                     [pair](bool ok, const QString &path, const QString &message) {
                         ++pair->result.count;
                         pair->result.ok      = ok;
                         pair->result.path    = path;
                         pair->result.message = message;
                     });

    return true;
}

// 断言落盘文件与源文件一致（这就是验收标准）
void verifyDelivered(const QString &receivedPath,
                     const QString &sourceDirName,
                     const QByteArray &content)
{
    QVERIFY(!receivedPath.isEmpty());
    QCOMPARE(QFileInfo(receivedPath).fileName(), sourceDirName);
    QVERIFY2(QFile::exists(receivedPath), qPrintable(receivedPath));

    const QByteArray landed = readAll(receivedPath);
    QCOMPARE(landed.size(), content.size());
    QCOMPARE(lanrdp::sha256Of(landed), lanrdp::sha256Of(content));
    QCOMPARE(landed, content);
}

} // namespace

void TestFileTransferE2E::transfersFileAcrossMultipleChunks()
{
    QTemporaryDir sendDir;
    QTemporaryDir recvDir;
    QVERIFY(sendDir.isValid() && recvDir.isValid());

    const QByteArray content = makeContent(lanrdp::kFileChunkSize * 2 + 4321);
    const QString    path    = writeTempFile(sendDir, QStringLiteral("photo.bin"), content);
    QVERIFY(!path.isEmpty());

    Pair pair;
    QVERIFY(setup(&pair, recvDir.path()));

    lanrdp::FileSender sender(pair.senderSide.get());
    QVERIFY(sender.start(path, 1));

    QTRY_COMPARE_WITH_TIMEOUT(pair.result.count, 1, 10000);
    QVERIFY2(pair.result.ok, qPrintable(pair.result.message));

    verifyDelivered(pair.result.path, QStringLiteral("photo.bin"), content);

    // 必须落在接收目录里
    QCOMPARE(QFileInfo(pair.result.path).absolutePath(),
             QFileInfo(recvDir.path()).absoluteFilePath());
}

void TestFileTransferE2E::transfersLargeFileWithBackpressure()
{
    QTemporaryDir sendDir;
    QTemporaryDir recvDir;
    QVERIFY(sendDir.isValid() && recvDir.isValid());

    // 8 MiB：远超 4 MiB 的写缓冲高水位与单次 pump 预算，
    // 会真实走到 bytesWritten 唤醒与让出事件循环两条路径。
    const QByteArray content = makeContent(8 * 1024 * 1024 + 999);
    const QString    path    = writeTempFile(sendDir, QStringLiteral("big.tar"), content);
    QVERIFY(!path.isEmpty());

    Pair pair;
    QVERIFY(setup(&pair, recvDir.path()));

    lanrdp::FileSender sender(pair.senderSide.get());
    QVERIFY(sender.start(path, 5));

    QTRY_COMPARE_WITH_TIMEOUT(pair.result.count, 1, 30000);
    QVERIFY2(pair.result.ok, qPrintable(pair.result.message));

    verifyDelivered(pair.result.path, QStringLiteral("big.tar"), content);
}

void TestFileTransferE2E::transfersTwoFilesBackToBack()
{
    QTemporaryDir sendDir;
    QTemporaryDir recvDir;
    QVERIFY(sendDir.isValid() && recvDir.isValid());

    const QByteArray first  = makeContent(lanrdp::kFileChunkSize + 17);
    const QByteArray second = makeContent(1234);
    const QString    pathA  = writeTempFile(sendDir, QStringLiteral("a.txt"), first);
    const QString    pathB  = writeTempFile(sendDir, QStringLiteral("b.txt"), second);
    QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

    Pair pair;
    QVERIFY(setup(&pair, recvDir.path()));

    lanrdp::FileSender sender(pair.senderSide.get());

    QVERIFY(sender.start(pathA, 31));
    QTRY_COMPARE_WITH_TIMEOUT(pair.result.count, 1, 10000);
    QVERIFY2(pair.result.ok, qPrintable(pair.result.message));
    const QString pathAReceived = pair.result.path;

    QVERIFY(sender.start(pathB, 32));
    QTRY_COMPARE_WITH_TIMEOUT(pair.result.count, 2, 10000);
    QVERIFY2(pair.result.ok, qPrintable(pair.result.message));
    const QString pathBReceived = pair.result.path;

    verifyDelivered(pathAReceived, QStringLiteral("a.txt"), first);
    verifyDelivered(pathBReceived, QStringLiteral("b.txt"), second);
    QVERIFY(pathAReceived != pathBReceived);
}

// 两条连接**同时**往同一个接收器传文件 —— 该场景在真机上就是
// 「两个控制端一起拖文件给被控端」。
//
// 病灶：接收器曾是全局单实例、状态只有一份，第二条连接一发 HEADER
// 就把第一条正在收的文件判失败并删掉。下面这条用真实 socket 把它钉死。
void TestFileTransferE2E::twoConnectionsTransferConcurrently()
{
    QTemporaryDir sendDirA;
    QTemporaryDir sendDirB;
    QTemporaryDir recvDir;
    QVERIFY(sendDirA.isValid() && sendDirB.isValid() && recvDir.isValid());

    // 都用大文件：让两路传输**真的重叠**（小文件会一路先跑完，测不到并发）
    const QByteArray contentA = makeContent(6 * 1024 * 1024 + 777);
    const QByteArray contentB = makeContent(6 * 1024 * 1024 + 333);
    const QString    pathA    = writeTempFile(sendDirA, QStringLiteral("a.bin"), contentA);
    const QString    pathB    = writeTempFile(sendDirB, QStringLiteral("b.bin"), contentB);
    QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

    // 两条独立的 loopback 连接，**共用一个 FileReceiver**（正是生产的拓扑）
    Pair pairA, pairB;
    QVERIFY(setup(&pairA, recvDir.path()));

    // 第二条连接要自己建 server/session，但接收器换成 pairA 那一个。
    // 做法：把 pairB 的接收侧帧也喂给 pairA 的接收器（来源不同）。
    QVERIFY(pairB.server.listen(QHostAddress::LocalHost, 0));
    auto *clientSocketB = new QTcpSocket();
    clientSocketB->connectToHost(QHostAddress::LocalHost, pairB.server.serverPort());
    QVERIFY(clientSocketB->waitForConnected(3000));
    QVERIFY(pairB.server.waitForNewConnection(3000));
    QTcpSocket *serverSideB = pairB.server.nextPendingConnection();
    QVERIFY(serverSideB);
    pairB.senderSide   = std::make_unique<lanrdp::Session>(clientSocketB);
    pairB.receiverSide = std::make_unique<lanrdp::Session>(serverSideB);

    QObject::connect(pairB.receiverSide.get(), &lanrdp::Session::frameReceived,
                     pairA.receiver.get(),
                     [&pairA, &pairB](const lanrdp::Frame &frame) {
                         pairA.receiver->onFrame(pairB.receiverSide.get(), frame);
                     });

    // 两路都开跑（间隔极小，保证重叠）
    lanrdp::FileSender senderA(pairA.senderSide.get());
    lanrdp::FileSender senderB(pairB.senderSide.get());

    // 收集每次 finished 的成败（共该有 2 次）
    QVector<bool> outcomes;
    QObject::connect(pairA.receiver.get(), &lanrdp::FileReceiver::finished,
                     pairA.receiver.get(),
                     [&outcomes](bool ok, const QString &, const QString &) {
                         outcomes.append(ok);
                     });

    QVERIFY(senderA.start(pathA, 101));
    QVERIFY(senderB.start(pathB, 102));

    QTRY_COMPARE_WITH_TIMEOUT(outcomes.size(), 2, 30000);

    // 两路都必须成功 —— 旧实现下先到的那个会被后到的 HEADER 打断
    // （其中一次 ok==false，且它的落盘文件被删）。
    for (bool ok : outcomes)
        QVERIFY2(ok, "并发传输时两路都该成功收完");

    // 两个文件都要完整落盘，内容各自对得上
    QVector<QByteArray> landed;
    const QDir dir(recvDir.path());
    for (const QString &n : dir.entryList(QDir::Files))
        landed.append(readAll(dir.absoluteFilePath(n)));

    QCOMPARE(landed.size(), 2);
    bool sawA = false, sawB = false;
    for (const QByteArray &d : landed) {
        if (lanrdp::sha256Of(d) == lanrdp::sha256Of(contentA))
            sawA = true;
        if (lanrdp::sha256Of(d) == lanrdp::sha256Of(contentB))
            sawB = true;
    }
    QVERIFY2(sawA, "A 的文件必须完整落盘（不该被 B 的 HEADER 打断）");
    QVERIFY2(sawB, "B 的文件必须完整落盘");
}

// 中途断链 —— 生产里「控制端掉线 / 网线被拔」走的就是这条路，
// 而旧用例全在良性路径上（半截文件清理只靠直接调 abort() 覆盖，等于没测过断开路径）。
//
// 病灶：接收端把「没等到 END」的半截文件当成品留在磁盘上 —— 用户拿到一个名字完整、
// 内容残缺的文件，比什么都没有更危险（尤其它会占住同名文件的位置）。
void TestFileTransferE2E::midTransferDisconnectLeavesNoPartialFile()
{
    QTemporaryDir sendDir;
    QTemporaryDir recvDir;
    QVERIFY(sendDir.isValid() && recvDir.isValid());

    // 大文件：确保断链**一定**落在传输中途。小文件可能一瞬间就发完，
    // 那样测到的是「传完了才断」，不是本用例要覆盖的路径。
    const QByteArray content = makeContent(8 * 1024 * 1024 + 999);
    const QString    path    = writeTempFile(sendDir, QStringLiteral("partial.bin"), content);
    QVERIFY(!path.isEmpty());

    Pair pair;
    QVERIFY(setup(&pair, recvDir.path()));
    QVERIFY(pair.clientSocket != nullptr);

    lanrdp::FileSender sender(pair.senderSide.get());
    QVERIFY(sender.start(path, 7));

    // 等到确实开始落盘（磁盘上出现半截文件）再掐链路
    const QDir dir(recvDir.path());
    QTRY_VERIFY_WITH_TIMEOUT(!dir.entryList(QDir::Files).isEmpty(), 15000);

    // 硬断（RST）：等价于对端进程消失
    pair.clientSocket->abort();

    // ① 接收端必须明确结掉这次传输，而不是一直挂着等永远不来的 END
    QTRY_VERIFY_WITH_TIMEOUT(pair.result.count > 0, 15000);
    QVERIFY2(!pair.result.ok, "中途断链不该被当作传输成功");

    // ② 磁盘上不得留下半截文件 —— 这条是本用例的核心
    QTRY_COMPARE_WITH_TIMEOUT(dir.entryList(QDir::Files).size(), 0, 15000);
    QVERIFY2(dir.entryList(QDir::Files).isEmpty(), "中途断链后不该留下骗人的半截文件");
}

QTEST_GUILESS_MAIN(TestFileTransferE2E)
#include "test_filetransfer_e2e.moc"
