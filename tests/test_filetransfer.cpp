#include <QtTest/QtTest>

#include <QByteArray>
#include <QSet>
#include <QString>

#include "protocol/FileTransfer.h"

// FILE_HEADER / FILE_CHUNK / FILE_END 载荷编解码
//
// 布局（全部小端）：
// FILE_HEADER = [transferId u32][nameLen u16][name UTF-8][totalSize u64]
// FILE_CHUNK = [transferId u32][offset u64][data ...]
// FILE_END = [transferId u32][bytesSent u64][sha256 32B]
class TestFileTransfer : public QObject
{
    Q_OBJECT

private slots:
    void headerRoundTrip();
    void chunkRoundTrip();
    void endRoundTrip();

    void headerRejectsBogus();
    void chunkRejectsBogus();
    void chunkRejectsOversizedPayload();
    void endRejectsBogus();

    void sanitizeStripsPathTraversal();
    void sanitizeHandlesReservedAndEmptyNames();
    void uniquifyAppendsCounter();
    void uniquifyGivesUpWhenAllNamesTaken();
    void sha256MatchesKnownVector();
};

// ---------------------------------------------------------------------------
// round-trip
// ---------------------------------------------------------------------------

void TestFileTransfer::headerRoundTrip()
{
    lanrdp::FileHeader header;
    header.transferId = 42;
    header.name       = QStringLiteral("项目说明 v2.tar.gz");
    header.totalSize  = 5ull * 1024 * 1024 * 1024;  // 5 GiB，超过 u32

    const QByteArray payload = lanrdp::encodeFileHeader(header);
    QVERIFY(!payload.isEmpty());

    const auto decoded = lanrdp::decodeFileHeader(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->transferId, 42u);
    QCOMPARE(decoded->name, header.name);
    QCOMPARE(decoded->totalSize, header.totalSize);
}

void TestFileTransfer::chunkRoundTrip()
{
    // 含 NUL 与高位字节的二进制数据，确保不是按字符串处理
    QByteArray data;
    data.append('\0');
    data.append("\xff\xfe\x01", 3);
    data.append("hello", 5);

    lanrdp::FileChunk chunk;
    chunk.transferId = 7;
    chunk.offset     = 0x1'0000'0000ull;  // 超过 u32 的偏移
    chunk.data       = data;

    const QByteArray payload = lanrdp::encodeFileChunk(chunk);
    QCOMPARE(payload.size(), int(lanrdp::kFileChunkHeaderSize + data.size()));

    const auto decoded = lanrdp::decodeFileChunk(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->transferId, 7u);
    QCOMPARE(decoded->offset, 0x1'0000'0000ull);
    QCOMPARE(decoded->data, data);
}

void TestFileTransfer::endRoundTrip()
{
    lanrdp::FileEnd end;
    end.transferId = 9;
    end.bytesSent  = 123456789;
    end.sha256     = QByteArray(lanrdp::kSha256Size, '\x5a');

    const QByteArray payload = lanrdp::encodeFileEnd(end);
    QCOMPARE(payload.size(), int(lanrdp::kFileEndSize));

    const auto decoded = lanrdp::decodeFileEnd(payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->transferId, 9u);
    QCOMPARE(decoded->bytesSent, 123456789ull);
    QCOMPARE(decoded->sha256, end.sha256);
}

// ---------------------------------------------------------------------------
// 边界防御
// ---------------------------------------------------------------------------

void TestFileTransfer::headerRejectsBogus()
{
    const QByteArray good = lanrdp::encodeFileHeader(
        lanrdp::FileHeader{1, QStringLiteral("a.txt"), 10});

    QVERIFY(!lanrdp::decodeFileHeader(QByteArray()).has_value());
    QVERIFY(!lanrdp::decodeFileHeader(good.left(int(lanrdp::kFileHeaderFixedSize) - 1)).has_value());

    // nameLen 声称比实际剩余字节多（截断）
    QByteArray truncated = good;
    const int nameLenOffset = 4;
    truncated[nameLenOffset]     = char(0xff);
    truncated[nameLenOffset + 1] = char(0xff);  // nameLen = 65535
    QVERIFY(!lanrdp::decodeFileHeader(truncated).has_value());

    // 多余的尾巴（长度不吻合）也应被拒
    QByteArray padded = good;
    padded.append('\0');
    QVERIFY(!lanrdp::decodeFileHeader(padded).has_value());

    // nameLen = 0（没有文件名）
    QByteArray noName = good;
    noName[nameLenOffset]     = '\0';
    noName[nameLenOffset + 1] = '\0';
    QVERIFY(!lanrdp::decodeFileHeader(noName).has_value());

    // totalSize = 0
    QByteArray zeroSize = good;
    zeroSize[zeroSize.size() - 8] = '\0';
    zeroSize[zeroSize.size() - 7] = '\0';
    zeroSize[zeroSize.size() - 6] = '\0';
    zeroSize[zeroSize.size() - 5] = '\0';
    zeroSize[zeroSize.size() - 4] = '\0';
    zeroSize[zeroSize.size() - 3] = '\0';
    zeroSize[zeroSize.size() - 2] = '\0';
    zeroSize[zeroSize.size() - 1] = '\0';
    QVERIFY(!lanrdp::decodeFileHeader(zeroSize).has_value());
}

void TestFileTransfer::chunkRejectsBogus()
{
    const QByteArray good = lanrdp::encodeFileChunk(
        lanrdp::FileChunk{1, 0, QByteArrayLiteral("data")});

    QVERIFY(!lanrdp::decodeFileChunk(QByteArray()).has_value());
    QVERIFY(!lanrdp::decodeFileChunk(good.left(int(lanrdp::kFileChunkHeaderSize) - 1)).has_value());

    // 只有头、没有数据（空块）
    QVERIFY(!lanrdp::decodeFileChunk(good.left(int(lanrdp::kFileChunkHeaderSize))).has_value());
}

// 块长必须受限：kFileChunkSize 原本只是**发送端**约定，接收端不校验的话，对端就能塞
// 任意大的块进来（帧层 32 MiB 只是「流已损坏」的门槛，不是「块」的语义约束）。
void TestFileTransfer::chunkRejectsOversizedPayload()
{
    // 恰好等于约定块长：合法
    const QByteArray ok = lanrdp::encodeFileChunk(
        lanrdp::FileChunk{1, 0, QByteArray(lanrdp::kFileChunkSize, 'x')});
    QVERIFY2(lanrdp::decodeFileChunk(ok).has_value(), "恰好等于约定块长的块应当放行");

    // 超一块：必须拒
    const QByteArray tooBig = lanrdp::encodeFileChunk(
        lanrdp::FileChunk{1, 0, QByteArray(lanrdp::kFileChunkSize + 1, 'x')});
    QVERIFY2(!lanrdp::decodeFileChunk(tooBig).has_value(), "超过约定块长的块必须被拒");
}

void TestFileTransfer::endRejectsBogus()
{
    const QByteArray good = lanrdp::encodeFileEnd(
        lanrdp::FileEnd{1, 10, QByteArray(lanrdp::kSha256Size, '\x11')});

    QVERIFY(!lanrdp::decodeFileEnd(QByteArray()).has_value());
    QVERIFY(!lanrdp::decodeFileEnd(good.left(int(lanrdp::kFileEndSize) - 1)).has_value());
    QVERIFY(!lanrdp::decodeFileEnd(good + '\0').has_value());
}

// ---------------------------------------------------------------------------
// 文件名清洗（接收端会收到对端给的任意文件名，必须防目录穿越）
// ---------------------------------------------------------------------------

void TestFileTransfer::sanitizeStripsPathTraversal()
{
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("../../evil.txt")),
             QStringLiteral("evil.txt"));
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("/etc/passwd")),
             QStringLiteral("passwd"));
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("C:\\Windows\\System32\\x.dll")),
             QStringLiteral("x.dll"));
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("..\\..\\..\\evil")),
             QStringLiteral("evil"));

    // 非法字符被剔除
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("a<b>c:d|e?f*g\"h.txt")),
             QStringLiteral("abcdefgh.txt"));

    // 正常名字（含中文）保持不变
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("项目说明 v2.tar.gz")),
             QStringLiteral("项目说明 v2.tar.gz"));
}

void TestFileTransfer::sanitizeHandlesReservedAndEmptyNames()
{
    // 目录本身 / 空名 -> 回退
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral(".")), QStringLiteral("file"));
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("..")), QStringLiteral("file"));
    QCOMPARE(lanrdp::sanitizeFileName(QString()), QStringLiteral("file"));
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("   ")), QStringLiteral("file"));
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("...")), QStringLiteral("file"));

    // Windows 设备名不能直接落盘（写 CON 会挂住），必须改名
    QVERIFY(lanrdp::sanitizeFileName(QStringLiteral("CON")) != QStringLiteral("CON"));
    QVERIFY(lanrdp::sanitizeFileName(QStringLiteral("con.txt")) != QStringLiteral("con.txt"));
    QVERIFY(lanrdp::sanitizeFileName(QStringLiteral("NUL")) != QStringLiteral("NUL"));

    // ⚠️ Windows 判定保留名时忽略基本名尾部的空格 / 点 —— "CON .txt" 等价 "CON"，
    //    必须同样改名，否则这个变体能绕过去直接落盘。
    QVERIFY(lanrdp::sanitizeFileName(QStringLiteral("CON .txt")).startsWith(QLatin1Char('_')));
    QVERIFY(lanrdp::sanitizeFileName(QStringLiteral("nul .log")) != QStringLiteral("nul .log"));
    QVERIFY(lanrdp::sanitizeFileName(QStringLiteral("COM1 .dat")).startsWith(QLatin1Char('_')));

    // 结尾的点会被 Windows 吃掉，应提前去掉
    QCOMPARE(lanrdp::sanitizeFileName(QStringLiteral("report.txt.")),
             QStringLiteral("report.txt"));
}

void TestFileTransfer::uniquifyAppendsCounter()
{
    // 没人占用时原样返回
    QCOMPARE(lanrdp::uniquifyFileName(QStringLiteral("a.txt"),
                                      [](const QString &) { return false; }),
             QStringLiteral("a.txt"));

    // 已占用 -> 依次尝试 -1 / -2
    QSet<QString> taken{QStringLiteral("a.txt"), QStringLiteral("a-1.txt")};
    QCOMPARE(lanrdp::uniquifyFileName(QStringLiteral("a.txt"),
                                      [&taken](const QString &n) { return taken.contains(n); }),
             QStringLiteral("a-2.txt"));

    // 扩展名应留在末尾，而不是变成 a.tar-1.gz
    QCOMPARE(lanrdp::uniquifyFileName(QStringLiteral("a.tar.gz"),
                                      [](const QString &n) { return n == QStringLiteral("a.tar.gz"); }),
             QStringLiteral("a.tar-1.gz"));

    // 没有扩展名时直接追加
    QCOMPARE(lanrdp::uniquifyFileName(QStringLiteral("README"),
                                      [](const QString &n) { return n == QStringLiteral("README"); }),
             QStringLiteral("README-1"));
}

// 候选名全部被占用时**必须返回空**，
// 由调用方转成「传输失败」。
// 旧实现在 i 循环跑满后 `return name;` —— 回落到**已被占用**的原名，接收端随后
// 用 WriteOnly|Truncate 打开，等于把用户已有的同名文件清空后写入对端内容。
// 这不是「名字不好看」，是**静默的数据丢失**。
void TestFileTransfer::uniquifyGivesUpWhenAllNamesTaken()
{
    const QString result = lanrdp::uniquifyFileName(
        QStringLiteral("a.txt"), [](const QString &) { return true; });

    QVERIFY2(result.isEmpty(),
             "候选名全被占满时应当放弃（返回空），绝不能回落到原名去覆盖已有文件");
}

void TestFileTransfer::sha256MatchesKnownVector()
{
    // SHA-256("abc")
    const QByteArray expected =
        QByteArray::fromHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    QCOMPARE(lanrdp::sha256Of(QByteArrayLiteral("abc")), expected);
    QCOMPARE(expected.size(), lanrdp::kSha256Size);
}

QTEST_GUILESS_MAIN(TestFileTransfer)
#include "test_filetransfer.moc"
