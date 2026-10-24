#include <QtTest/QtTest>

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSignalSpy>
#include <QUrl>

#include "viewer/ScreenRenderer.h"

// 从拖拽的 MIME 数据里挑出「第一个本地文件」。
// 抽成纯函数是为了脱离事件循环单测（与 mapToNormalized / planInjection 同风格）。
class TestRendererDrop : public QObject
{
    Q_OBJECT

private slots:
    void picksLocalFileFromFileUrl();
    void rejectsPlainText();
    void rejectsEmptyMimeData();
    void rejectsNullMimeData();
    void picksFirstLocalFileFromMixedList();
    void skipsRemoteUrlsBeforeLocalFile();
    void rejectsRemoteOnlyList();

    void dragEnterAcceptedOnlyForLocalFile();
    void dropEmitsFileDroppedWithPath();
    void dropWithoutFileEmitsNothing();
};

namespace {

QMimeData *makeUrls(const QList<QUrl> &urls)
{
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    return mime;
}

} // namespace

void TestRendererDrop::picksLocalFileFromFileUrl()
{
    QScopedPointer<QMimeData> mime(makeUrls({QUrl::fromLocalFile(QStringLiteral("C:/a/b.txt"))}));

    QCOMPARE(lanrdp::ScreenRenderer::firstLocalFile(mime.data()),
             QStringLiteral("C:/a/b.txt"));
}

void TestRendererDrop::rejectsPlainText()
{
    QScopedPointer<QMimeData> mime(new QMimeData);
    mime->setText(QStringLiteral("C:/a/b.txt"));

    // 纯文本里即使长得像路径也不是文件拖拽
    QVERIFY(lanrdp::ScreenRenderer::firstLocalFile(mime.data()).isEmpty());
}

void TestRendererDrop::rejectsEmptyMimeData()
{
    QScopedPointer<QMimeData> mime(new QMimeData);
    QVERIFY(lanrdp::ScreenRenderer::firstLocalFile(mime.data()).isEmpty());
}

void TestRendererDrop::rejectsNullMimeData()
{
    QVERIFY(lanrdp::ScreenRenderer::firstLocalFile(nullptr).isEmpty());
}

void TestRendererDrop::picksFirstLocalFileFromMixedList()
{
    QScopedPointer<QMimeData> mime(makeUrls({
        QUrl::fromLocalFile(QStringLiteral("C:/a/one.txt")),
        QUrl::fromLocalFile(QStringLiteral("C:/a/two.txt")),
    }));

    // 多文件只取第一个（FileSender 是单文件模型）
    QCOMPARE(lanrdp::ScreenRenderer::firstLocalFile(mime.data()),
             QStringLiteral("C:/a/one.txt"));
}

void TestRendererDrop::skipsRemoteUrlsBeforeLocalFile()
{
    QScopedPointer<QMimeData> mime(makeUrls({
        QUrl(QStringLiteral("https://example.com/x.png")),
        QUrl::fromLocalFile(QStringLiteral("D:/tmp/本地 文件.bin")),
    }));

    QCOMPARE(lanrdp::ScreenRenderer::firstLocalFile(mime.data()),
             QStringLiteral("D:/tmp/本地 文件.bin"));
}

void TestRendererDrop::rejectsRemoteOnlyList()
{
    QScopedPointer<QMimeData> mime(makeUrls({QUrl(QStringLiteral("https://example.com/x.png"))}));
    QVERIFY(lanrdp::ScreenRenderer::firstLocalFile(mime.data()).isEmpty());
}

// ---- 事件层：验证接线（信号 + 事件接受策略），纯函数测试覆盖不到这一段 ----

namespace {

// 把 protected 的 dropEvent 提为 public。
// 实测（Qt 6.11.2）：QApplication::sendEvent 能把 DragEnter 送到控件上，
// 但合成的 QEvent::Drop 送不到 QWidget::dropEvent（spy 为 0、accepted 为假），
// 所以这里直接调 handler，验的是 handler 自身行为；真实拖放的路由由
// Qt 的 drag manager 负责，不在单测范围内。
class ProbeRenderer : public lanrdp::ScreenRenderer
{
public:
    using lanrdp::ScreenRenderer::dropEvent;
};

} // namespace

void TestRendererDrop::dragEnterAcceptedOnlyForLocalFile()
{
    lanrdp::ScreenRenderer renderer;
    QVERIFY(renderer.acceptDrops());

    QScopedPointer<QMimeData> fileMime(makeUrls({QUrl::fromLocalFile(QStringLiteral("C:/a/b.txt"))}));
    QDragEnterEvent accept(QPoint(10, 10), Qt::CopyAction, fileMime.data(),
                           Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&renderer, &accept);
    QVERIFY2(accept.isAccepted(), "本地文件拖入应当被接受");

    QScopedPointer<QMimeData> textMime(new QMimeData);
    textMime->setText(QStringLiteral("just text"));
    QDragEnterEvent reject(QPoint(10, 10), Qt::CopyAction, textMime.data(),
                           Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&renderer, &reject);
    QVERIFY2(!reject.isAccepted(), "纯文本拖入不应被接管");
}

void TestRendererDrop::dropEmitsFileDroppedWithPath()
{
    ProbeRenderer renderer;
    QSignalSpy spy(&renderer, &lanrdp::ScreenRenderer::fileDropped);

    QScopedPointer<QMimeData> mime(makeUrls({
        QUrl::fromLocalFile(QStringLiteral("C:/a/one.txt")),
        QUrl::fromLocalFile(QStringLiteral("C:/a/two.txt")),
    }));

    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, mime.data(),
                    Qt::LeftButton, Qt::NoModifier);
    renderer.dropEvent(&drop);

    QVERIFY(drop.isAccepted());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("C:/a/one.txt"));
}

void TestRendererDrop::dropWithoutFileEmitsNothing()
{
    ProbeRenderer renderer;
    QSignalSpy spy(&renderer, &lanrdp::ScreenRenderer::fileDropped);

    QScopedPointer<QMimeData> mime(new QMimeData);
    mime->setText(QStringLiteral("just text"));

    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, mime.data(),
                    Qt::LeftButton, Qt::NoModifier);
    renderer.dropEvent(&drop);

    QVERIFY(!drop.isAccepted());
    QCOMPARE(spy.count(), 0);
}

QTEST_MAIN(TestRendererDrop)
#include "test_rendererdrop.moc"
