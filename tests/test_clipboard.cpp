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

QTEST_GUILESS_MAIN(TestClipboard)
#include "test_clipboard.moc"
