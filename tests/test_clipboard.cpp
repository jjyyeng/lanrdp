#include <QtTest/QtTest>

#include <QByteArray>
#include <QString>

#include "protocol/Clipboard.h"

// CLIPBOARD_TEXT 载荷编解码
//
// 布局：载荷 = 裸 UTF-8 文本（**没有**头部字段）。
// 从前这里带一个 u32 `seq`，但它从不参与排序/陈旧判定，只误导维护者 —— v3 删掉了。
class TestClipboard : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip();
    void emptyPayloadMeansClear();
    void rejectsOversizedText();
    void toleratesInvalidUtf8();
    void boundaryAtMaxSize();

    // CLIPBOARD_FILES（v6）
    void filesRoundTrip();
    void filesRejectsBadInput();
    void filesRejectsMalformedPayload();
};

void TestClipboard::roundTrip()
{
    const QString text = QStringLiteral("你好，世界 — hello\n\ttab");
    const auto    payload = lanrdp::encodeClipboardText(text);
    QVERIFY(payload.has_value());
    QCOMPARE(payload->size(), text.toUtf8().size());   // 载荷就是文本本身，无多余字段

    const auto decoded = lanrdp::decodeClipboardText(*payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->text, text);
}

// 空文本 / 空载荷是合法载荷（语义：清空剪贴板），不是错误。
void TestClipboard::emptyPayloadMeansClear()
{
    const auto payload = lanrdp::encodeClipboardText(QString());
    QVERIFY(payload.has_value());
    QVERIFY(payload->isEmpty());

    const auto decoded = lanrdp::decodeClipboardText(*payload);
    QVERIFY(decoded.has_value());
    QVERIFY(decoded->text.isEmpty());

    // 空载荷（对端发的 0 字节 CLIPBOARD_TEXT）同样解成空文本
    const auto fromEmpty = lanrdp::decodeClipboardText(QByteArray());
    QVERIFY(fromEmpty.has_value());
    QVERIFY(fromEmpty->text.isEmpty());
}

void TestClipboard::rejectsOversizedText()
{
    // 1 MiB + 1 字节（纯 ASCII 下字节数 == 字符数）
    const QString tooBig(static_cast<int>(lanrdp::kClipboardMaxBytes) + 1, QLatin1Char('a'));
    QVERIFY(!lanrdp::encodeClipboardText(tooBig).has_value());

    // decode 侧同样拒绝（防止对端发来超限载荷）
    const QByteArray payload(static_cast<int>(lanrdp::kClipboardMaxBytes) + 1, 'a');
    QVERIFY(!lanrdp::decodeClipboardText(payload).has_value());
}

void TestClipboard::toleratesInvalidUtf8()
{
    const QByteArray payload = QByteArrayLiteral("\xff\xfe\x80\xc3");   // 非法 UTF-8 序列

    const auto decoded = lanrdp::decodeClipboardText(payload);
    QVERIFY(decoded.has_value());       // 不崩、不算失败
    QVERIFY(!decoded->text.isNull());   // fromUtf8 容错为替换字符
}

void TestClipboard::boundaryAtMaxSize()
{
    // 正好等于上限应可通过（边界不误杀）
    const QString exact(static_cast<int>(lanrdp::kClipboardMaxBytes), QLatin1Char('x'));
    const auto    payload = lanrdp::encodeClipboardText(exact);
    QVERIFY(payload.has_value());
    QCOMPARE(payload->size(), int(lanrdp::kClipboardMaxBytes));

    const auto decoded = lanrdp::decodeClipboardText(*payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->text.size(), int(lanrdp::kClipboardMaxBytes));
}

// CLIPBOARD_FILES：文件清单往返。名字是**文件名**（不带目录），中文名必须原样活下来 ——
// 接收侧靠 parse 后的名字给人看，坏掉就是「粘贴出来文件名是乱码」。
void TestClipboard::filesRoundTrip()
{
    const QStringList names{QStringLiteral("报告.docx"),
                            QStringLiteral("vacation photo.jpg"),
                            QStringLiteral("a.txt")};

    const auto payload = lanrdp::encodeClipboardFiles(names);
    QVERIFY(payload.has_value());

    const auto decoded = lanrdp::decodeClipboardFiles(*payload);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->names, names);

    // 单个文件也要能编，且载荷 = 2 字节个数 + 2 字节长度 + 名字本身
    const auto single = lanrdp::encodeClipboardFiles({QStringLiteral("a.txt")});
    QVERIFY(single.has_value());
    QCOMPARE(single->size(), 2 + 2 + 5);
}

// 编不出来的一律整批拒绝，绝不发半批：对方按清单等文件，少一个就永远等不齐。
void TestClipboard::filesRejectsBadInput()
{
    QVERIFY(!lanrdp::encodeClipboardFiles({}).has_value());
    QVERIFY(!lanrdp::encodeClipboardFiles({QStringLiteral("ok.txt"), QString()}).has_value());

    QStringList tooMany;
    for (int i = 0; i <= lanrdp::kClipboardMaxFiles; ++i)
        tooMany.append(QStringLiteral("f%1.txt").arg(i));
    QVERIFY(!lanrdp::encodeClipboardFiles(tooMany).has_value());

    // 正好等于上限仍可编（边界不误杀）
    QStringList exact;
    for (int i = 0; i < lanrdp::kClipboardMaxFiles; ++i)
        exact.append(QStringLiteral("f%1.txt").arg(i));
    QVERIFY(lanrdp::encodeClipboardFiles(exact).has_value());

    const QString tooLong(static_cast<int>(lanrdp::kClipboardMaxFileNameBytes) + 1,
                          QLatin1Char('n'));
    QVERIFY(!lanrdp::encodeClipboardFiles({tooLong}).has_value());
}

// 解码侧的硬边界：长度不自洽 / count 越界 / 名字越界都整帧丢弃（返回 nullopt），
// 不许「尽力而为」地解出一半 —— 半份清单会让接收侧按错误的个数等文件。
void TestClipboard::filesRejectsMalformedPayload()
{
    // 连个数都读不出来
    QVERIFY(!lanrdp::decodeClipboardFiles(QByteArray()).has_value());
    QVERIFY(!lanrdp::decodeClipboardFiles(QByteArrayLiteral("\x01")).has_value());

    // count = 0
    QVERIFY(!lanrdp::decodeClipboardFiles(QByteArrayLiteral("\x00\x00")).has_value());

    // count 声称 2 个，实际只有 1 个
    QByteArray onlyOne;
    onlyOne.append("\x02\x00", 2);
    onlyOne.append("\x03\x00", 2);
    onlyOne.append("abc", 3);
    QVERIFY(!lanrdp::decodeClipboardFiles(onlyOne).has_value());

    // 名字长度越界
    QByteArray hugeLen;
    hugeLen.append("\x01\x00", 2);
    hugeLen.append("\xff\xff", 2);
    hugeLen.append("abc", 3);
    QVERIFY(!lanrdp::decodeClipboardFiles(hugeLen).has_value());

    // count 越界：声称的个数超过上限就整帧丢弃。encode 侧已有 64/65 的边界断言
    // （filesRejectsBadInput），但**解码**这条是另一份代码 —— 对端可以随便写个数过来，
    // 少了这条断言，「按 65535 个去 reserve」就没有任何东西拦着。
    QByteArray tooManyNames;
    tooManyNames.append("\x41\x00", 2);   // 65，小端
    for (int i = 0; i < 65; ++i) {
        tooManyNames.append("\x01\x00", 2);
        tooManyNames.append("a", 1);
    }
    QVERIFY(!lanrdp::decodeClipboardFiles(tooManyNames).has_value());

    // 末尾多出字节：布局对不上，宁可丢弃
    QByteArray trailing;
    trailing.append("\x01\x00", 2);
    trailing.append("\x03\x00", 2);
    trailing.append("abc", 3);
    trailing.append("x", 1);
    QVERIFY(!lanrdp::decodeClipboardFiles(trailing).has_value());
}

QTEST_GUILESS_MAIN(TestClipboard)

#include "test_clipboard.moc"
