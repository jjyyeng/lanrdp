#include <QtTest>

#include "app/PasswordPolicy.h"

// 口令规则与随机生成的守门用例。
//
// 规则本身（长度 ≥ 6、仅 ASCII 0x21–0x7E）由 PasswordPolicy.h 唯一收口，
// 这里只钉「生成出来的串一定合规」，以及「不出现看错的字符」——
// 否则用户生成完抄给同事，抄错一个字符就是「口令不对」，排查起来极费劲。
class TestPasswordPolicy : public QObject
{
    Q_OBJECT

private slots:
    void acceptsTheDocumentedRule();
    void rejectsOutOfRuleInput();
    void generatedPasswordSatisfiesTheRule();
    void generatedPasswordAvoidsLookalikeCharacters();
    void generatedPasswordsDiffer();
    void honoursRequestedLength();
};

void TestPasswordPolicy::acceptsTheDocumentedRule()
{
    QVERIFY(lanrdp::isValidPassword(QStringLiteral("secret")));
    QVERIFY(lanrdp::isValidPassword(QStringLiteral("!@#$%^&*")));
    QVERIFY(lanrdp::isValidPassword(QStringLiteral("aB3!xY")));
}

void TestPasswordPolicy::rejectsOutOfRuleInput()
{
    QVERIFY(!lanrdp::isValidPassword(QString()));                 // 空
    QVERIFY(!lanrdp::isValidPassword(QStringLiteral("abcde")));   // 5 位
    QVERIFY(!lanrdp::isValidPassword(QStringLiteral("abc de")));  // 含空格
    QVERIFY(!lanrdp::isValidPassword(QStringLiteral("口令通行证")));  // 非 ASCII
}

void TestPasswordPolicy::generatedPasswordSatisfiesTheRule()
{
    for (int i = 0; i < 50; ++i) {
        const QString p = lanrdp::generatePassword();
        QVERIFY2(lanrdp::isValidPassword(p), qPrintable(QStringLiteral("不合规：") + p));
    }
}

// 0 / O、1 / l / I 这类字符在等宽字体里几乎同形。生成器一旦吐出它们，
// 「抄错一位」就成了最常见的使用故障 —— 而它看起来像口令本身错了。
void TestPasswordPolicy::generatedPasswordAvoidsLookalikeCharacters()
{
    const QString forbidden = QStringLiteral("0O1lI");
    for (int i = 0; i < 200; ++i) {
        const QString p = lanrdp::generatePassword();
        for (const QChar ch : p) {
            QVERIFY2(!forbidden.contains(ch),
                     qPrintable(QStringLiteral("出现易混字符：") + p));
        }
    }
}

// 同一个口令被生成两次的概率必须低到不可能观察到（否则等于没随机）。
void TestPasswordPolicy::generatedPasswordsDiffer()
{
    QVERIFY(lanrdp::generatePassword() != lanrdp::generatePassword());
}

void TestPasswordPolicy::honoursRequestedLength()
{
    QCOMPARE(lanrdp::generatePassword(8).size(), 8);
    QCOMPARE(lanrdp::generatePassword(20).size(), 20);
    // 兜底：请求比最小长度更短时，不能生成一个连规则都过不了的串。
    QVERIFY(lanrdp::isValidPassword(lanrdp::generatePassword(1)));
}

QTEST_MAIN(TestPasswordPolicy)
#include "test_passwordpolicy.moc"
