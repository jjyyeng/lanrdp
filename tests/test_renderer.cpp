#include <QtTest/QtTest>

#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSet>
#include <QSignalSpy>
#include <QWheelEvent>

#include "viewer/ScreenRenderer.h"

// 控制端画面渲染控件
class TestRenderer : public QObject
{
    Q_OBJECT

private slots:
    void scaledRectFitsAndKeepsAspect();
    void scaledRectLetterboxesWideWidget();
    void scaledRectHandlesDegenerateInput();
    void smoothScalingSkippedOnlyInDeadZone();
    void deadZoneSmoothingCanBeOverridden();
    void storesAndClearsFrame();
    // 「点此操作对方电脑」只对有远端的格子有意义（本机预览格必须不显示）
    void hintOnlyForCellsWithRemote();
    void pressOnHintRequestsEnableOnlyWhenHintShown();
    // 解码端把帧等比缩小后，归一化坐标映射必须不变
    void normalizedMappingIsScaleInvariant();

    // ---- 按下集合与补发（远端卡键）----
    // 「按住键 → 打断 → 抬起仍要发出」这条链是本组用例的核心。
    void pressAndReleaseAreSymmetric();
    void releaseAllPressedEmitsEveryPressedKey();
    void releaseAllPressedIsIdempotent();
    // 关开关 = 补发；且之后不再采集
    void disablingInputReleasesPressedKeys();
    // ⚠️ 失焦**不**补发（点悬浮条会失焦，误松键）
    void losingFocusDoesNotReleasePressedKeys();
    // 按键 / 滚轮载荷要带最后一次坐标
    void buttonAndWheelCarryLastPosition();
};

void TestRenderer::scaledRectFitsAndKeepsAspect()
{
    // 1920x1080 放进 960x600：缩放系数 0.5 -> 960x540，垂直居中偏上留 30px
    const QRect target = lanrdp::ScreenRenderer::scaledTargetRect(QSize(1920, 1080), QSize(960, 600));
    QCOMPARE(target.size(), QSize(960, 540));
    QCOMPARE(target.left(), 0);
    QCOMPARE(target.top(), 30);
}

void TestRenderer::scaledRectLetterboxesWideWidget()
{
    // 100x200 放进 400x200：缩放系数 1 -> 原尺寸，水平居中
    const QRect target = lanrdp::ScreenRenderer::scaledTargetRect(QSize(100, 200), QSize(400, 200));
    QCOMPARE(target.size(), QSize(100, 200));
    QCOMPARE(target.left(), 150);
    QCOMPARE(target.top(), 0);
}

void TestRenderer::scaledRectHandlesDegenerateInput()
{
    QVERIFY(lanrdp::ScreenRenderer::scaledTargetRect(QSize(0, 0), QSize(100, 100)).isNull());
    QVERIFY(lanrdp::ScreenRenderer::scaledTargetRect(QSize(10, 10), QSize(0, 0)).isNull());
}

void TestRenderer::smoothScalingSkippedOnlyInDeadZone()
{
    using lanrdp::ScreenRenderer;

    // 放大：必须开平滑，否则马赛克块明显。
    QVERIFY(ScreenRenderer::useSmoothScalingFor(QSize(800, 600), QSize(1600, 1200)));

    // 面积比 = 1（几乎不缩放）：死区 ⇒ 关（平滑对观感无益，纯烧 CPU）。
    QVERIFY(!ScreenRenderer::useSmoothScalingFor(QSize(900, 900), QSize(900, 900)));

    // 面积比 0.25（线性 0.5）：仍在死区 (1/9, 1] ⇒ 关。
    QVERIFY(!ScreenRenderer::useSmoothScalingFor(QSize(900, 900), QSize(450, 450)));
    // 略大于 1/9（面积比 ≈ 0.1186）仍在死区 ⇒ 关。
    QVERIFY(!ScreenRenderer::useSmoothScalingFor(QSize(900, 900), QSize(310, 310)));

    // 面积比 = 1/9 边界：大幅缩放 ⇒ 开（`<=` 含等号）。
    QVERIFY(ScreenRenderer::useSmoothScalingFor(QSize(900, 900), QSize(300, 300)));
    // 面积比 1/16：更大幅缩小 ⇒ 开。
    QVERIFY(ScreenRenderer::useSmoothScalingFor(QSize(900, 900), QSize(225, 225)));

    // 尺寸非法：兜底按画质优先。
    QVERIFY(ScreenRenderer::useSmoothScalingFor(QSize(0, 0), QSize(100, 100)));
    QVERIFY(ScreenRenderer::useSmoothScalingFor(QSize(100, 100), QSize(0, 0)));

    // 接线冒烟：死区尺寸下实际绘制不崩（判定生效与否由上面的纯函数覆盖）。
    ScreenRenderer renderer;
    QImage         image(3840, 2160, QImage::Format_ARGB32);
    image.fill(Qt::darkBlue);
    renderer.setFrame(image);
    renderer.resize(3600, 2025);  // 面积比 ≈ 0.879 ⇒ 死区
    renderer.repaint();
    QCOMPARE(renderer.frameSize(), QSize(3840, 2160));
}

void TestRenderer::deadZoneSmoothingCanBeOverridden()
{
    lanrdp::ScreenRenderer renderer;

    // 死区（面积比 0.25，线性 0.5）默认走省性能路径：不开平滑。
    QVERIFY(!renderer.smoothScalingFor(QSize(900, 900), QSize(450, 450)));

    // 一格独享 / 占满时允许在死区也开平滑（那种场景画面只有这一份要画，
    // 关掉会让文字发虚 —— 真机反馈「1 格看 2560×1440 不清楚」）。
    renderer.setSmoothInDeadZone(true);
    QVERIFY(renderer.smoothScalingFor(QSize(900, 900), QSize(450, 450)));

    // 总开关仍能一票否决（与现有 setSmoothScaling(false) 语义一致）。
    renderer.setSmoothScaling(false);
    QVERIFY(!renderer.smoothScalingFor(QSize(900, 900), QSize(450, 450)));
}

void TestRenderer::storesAndClearsFrame()
{
    lanrdp::ScreenRenderer renderer;
    QVERIFY(!renderer.hasFrame());
    QVERIFY(renderer.frameSize().isEmpty());

    QImage image(64, 48, QImage::Format_ARGB32);
    image.fill(Qt::green);
    renderer.setFrame(image);

    QVERIFY(renderer.hasFrame());
    QCOMPARE(renderer.frameSize(), QSize(64, 48));

    renderer.resize(200, 200);
    renderer.setSmoothScaling(false);
    QVERIFY(!renderer.smoothScaling());
    renderer.repaint();  // 触发 paintEvent，不应崩溃
    QCOMPARE(renderer.frameSize(), QSize(64, 48));

    renderer.clear();
    QVERIFY(!renderer.hasFrame());
    renderer.repaint();
}

void TestRenderer::normalizedMappingIsScaleInvariant()
{
    // 解码端会把帧**等比缩小**后再交给渲染器（省掉写全尺寸 ARGB32 的内存带宽）。
    // mapToNormalized 内部走 scaledTargetRect（纯比例计算），所以只要缩放在数学上等比，
    // 归一化坐标就与「未缩小的原始帧」完全一致 —— 否则鼠标落点会漂。
    const QSize widget(1280, 720);
    const QSize original(3840, 2160);
    const QSize halves(1920, 1080);   // 1/2
    const QSize thirds(1280, 720);    // 1/3

    const QRect base = lanrdp::ScreenRenderer::scaledTargetRect(original, widget);
    QCOMPARE(lanrdp::ScreenRenderer::scaledTargetRect(halves, widget), base);
    QCOMPARE(lanrdp::ScreenRenderer::scaledTargetRect(thirds, widget), base);

    const QPoint probes[] = {QPoint(0, 0), QPoint(640, 360), QPoint(1279, 719), QPoint(100, 500)};
    for (const QPoint &p : probes) {
        quint16 ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0;
        QVERIFY(lanrdp::ScreenRenderer::mapToNormalized(p, original, widget, &ax, &ay));
        QVERIFY(lanrdp::ScreenRenderer::mapToNormalized(p, halves, widget, &bx, &by));
        QVERIFY(lanrdp::ScreenRenderer::mapToNormalized(p, thirds, widget, &cx, &cy));
        QCOMPARE(bx, ax);
        QCOMPARE(by, ay);
        QCOMPARE(cx, ax);
        QCOMPARE(cy, ay);
    }
}

namespace {

// 造一个「有画面 + 已启用键鼠」的渲染控件，并让它有确定的尺寸。
void primeRenderer(lanrdp::ScreenRenderer &renderer)
{
    QImage image(1920, 1080, QImage::Format_ARGB32);
    image.fill(Qt::darkGray);
    renderer.setFrame(image);
    renderer.resize(960, 540);   // 正好 1:2，画面铺满整块控件
    renderer.setInputEnabled(true);
}

} // namespace

void TestRenderer::pressAndReleaseAreSymmetric()
{
    lanrdp::ScreenRenderer renderer;
    primeRenderer(renderer);

    QSignalSpy keys(&renderer, &lanrdp::ScreenRenderer::keyChanged);
    QSignalSpy buttons(&renderer, &lanrdp::ScreenRenderer::mouseButtonChanged);

    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
    QCoreApplication::sendEvent(&renderer, &press);
    QCoreApplication::sendEvent(&renderer, &release);

    QCOMPARE(keys.count(), 2);
    QCOMPARE(keys.at(0).at(1).toBool(), true);
    QCOMPARE(keys.at(1).at(1).toBool(), false);

    // Qt 6 的新构造函数要求同时给 local 与 global 位置（旧的重载已弃用）。
    QMouseEvent mPress(QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent mRelease(QEvent::MouseButtonRelease, QPointF(100, 100), QPointF(100, 100),
                         Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&renderer, &mPress);
    QCoreApplication::sendEvent(&renderer, &mRelease);

    QCOMPARE(buttons.count(), 2);
    QCOMPARE(buttons.at(0).at(1).toBool(), true);
    QCOMPARE(buttons.at(1).at(1).toBool(), false);
}

void TestRenderer::releaseAllPressedEmitsEveryPressedKey()
{
    lanrdp::ScreenRenderer renderer;
    primeRenderer(renderer);

    QSignalSpy keys(&renderer, &lanrdp::ScreenRenderer::keyChanged);
    QSignalSpy buttons(&renderer, &lanrdp::ScreenRenderer::mouseButtonChanged);

    // 按住两个键 + 一个鼠标键（模拟用户正按着 Ctrl+Shift 拖拽）
    QKeyEvent ctrlPress(QEvent::KeyPress, Qt::Key_Control, Qt::NoModifier);
    QKeyEvent shiftPress(QEvent::KeyPress, Qt::Key_Shift, Qt::NoModifier);
    QCoreApplication::sendEvent(&renderer, &ctrlPress);
    QCoreApplication::sendEvent(&renderer, &shiftPress);

    QMouseEvent mPress(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&renderer, &mPress);

    keys.clear();
    buttons.clear();

    renderer.releaseAllPressed();

    // 必须每个都补一次**抬起**
    QCOMPARE(keys.count(), 2);
    QCOMPARE(buttons.count(), 1);
    for (int i = 0; i < keys.count(); ++i)
        QCOMPARE(keys.at(i).at(1).toBool(), false);
    QCOMPARE(buttons.at(0).at(1).toBool(), false);

    // 抬起的是刚才按下的那些键（Ctrl / Shift）
    QSet<int> releasedVks;
    for (int i = 0; i < keys.count(); ++i)
        releasedVks.insert(keys.at(i).at(0).toInt());
    QCOMPARE(releasedVks.size(), 2);
}

void TestRenderer::releaseAllPressedIsIdempotent()
{
    lanrdp::ScreenRenderer renderer;
    primeRenderer(renderer);

    QSignalSpy keys(&renderer, &lanrdp::ScreenRenderer::keyChanged);

    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QCoreApplication::sendEvent(&renderer, &press);
    keys.clear();

    renderer.releaseAllPressed();
    QCOMPARE(keys.count(), 1);

    // 第二次（集合已空）应当是 no-op
    renderer.releaseAllPressed();
    QCOMPARE(keys.count(), 1);
}

void TestRenderer::disablingInputReleasesPressedKeys()
{
    // 症状：按住 Ctrl 时关掉「启用远程键鼠控制」⇒ 远端那个键**永久按下**。
    // 修法：`setInputEnabled(false)` 内部自己补发全部抬起（责任收在一处，
    // 「谁关的开关」都不必记得补）。
    lanrdp::ScreenRenderer renderer;
    primeRenderer(renderer);

    QSignalSpy keys(&renderer, &lanrdp::ScreenRenderer::keyChanged);

    QKeyEvent press(QEvent::KeyPress, Qt::Key_Control, Qt::NoModifier);
    QCoreApplication::sendEvent(&renderer, &press);
    QCOMPARE(keys.count(), 1);
    QCOMPARE(keys.at(0).at(1).toBool(), true);

    keys.clear();
    renderer.setInputEnabled(false);

    QCOMPARE(keys.count(), 1);
    QCOMPARE(keys.at(0).at(1).toBool(), false);

    // 再开回来时集合是干净的（不该凭空再补一次抬起）
    keys.clear();
    renderer.setInputEnabled(true);
    QCOMPARE(keys.count(), 0);
}

void TestRenderer::losingFocusDoesNotReleasePressedKeys()
{
    // ⚠️ 这是最容易改错的一处：Qt 里**点一下悬浮条**就会让画面控件失焦，
    // 如果失焦时补发抬起，用户**正常按住**的键会被莫名松掉。
    lanrdp::ScreenRenderer renderer;
    primeRenderer(renderer);

    QSignalSpy keys(&renderer, &lanrdp::ScreenRenderer::keyChanged);

    QKeyEvent press(QEvent::KeyPress, Qt::Key_Control, Qt::NoModifier);
    QCoreApplication::sendEvent(&renderer, &press);
    keys.clear();

    QFocusEvent focusOut(QEvent::FocusOut, Qt::MouseFocusReason);
    QCoreApplication::sendEvent(&renderer, &focusOut);

    QVERIFY2(keys.count() == 0, "失焦不应补发抬起 —— 点悬浮条会失焦，那会误松用户正按住的键");

    // 集合仍然记着这个键，真正的打断（关开关）时才会补
    renderer.releaseAllPressed();
    QCOMPARE(keys.count(), 1);
    QCOMPARE(keys.at(0).at(1).toBool(), false);
}

void TestRenderer::buttonAndWheelCarryLastPosition()
{
    lanrdp::ScreenRenderer renderer;
    primeRenderer(renderer);

    // 先移到一个确定的位置：(480, 270) 在 960x540 控件里 = 归一化 (32768, 32768) 附近
    QSignalSpy moves(&renderer, &lanrdp::ScreenRenderer::mouseMoved);
    QMouseEvent moveEv(QEvent::MouseMove, QPointF(480, 270), QPointF(480, 270),
                       Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&renderer, &moveEv);
    QCOMPARE(moves.count(), 1);
    const quint16 nx = moves.at(0).at(0).value<quint16>();
    const quint16 ny = moves.at(0).at(1).value<quint16>();

    // 按键/滚轮信号也必须带同一份坐标（新增的两个参数）
    QSignalSpy buttons(&renderer, &lanrdp::ScreenRenderer::mouseButtonChanged);
    QMouseEvent mPress(QEvent::MouseButtonPress, QPointF(480, 270), QPointF(480, 270),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&renderer, &mPress);
    QCOMPARE(buttons.count(), 1);
    QCOMPARE(buttons.at(0).count(), 4);  // button + pressed + nx + ny
    QCOMPARE(buttons.at(0).at(2).value<quint16>(), nx);
    QCOMPARE(buttons.at(0).at(3).value<quint16>(), ny);

    QSignalSpy wheels(&renderer, &lanrdp::ScreenRenderer::mouseWheel);
    QWheelEvent wheelEv(QPointF(480, 270), QPointF(480, 270), QPoint(), QPoint(0, 120),
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&renderer, &wheelEv);
    QCOMPARE(wheels.count(), 1);
    QCOMPARE(wheels.at(0).count(), 3);  // delta + nx + ny
    QCOMPARE(wheels.at(0).at(1).value<quint16>(), nx);
    QCOMPARE(wheels.at(0).at(2).value<quint16>(), ny);

    // 从没移过鼠标、直接点一下：载荷里也必须带**这一下自己的位置**，
    // 而不是 (0,0)（那是左上角，会让第一次点击点错位置）。
    lanrdp::ScreenRenderer fresh;
    primeRenderer(fresh);
    QSignalSpy freshButtons(&fresh, &lanrdp::ScreenRenderer::mouseButtonChanged);
    QMouseEvent firstClick(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&fresh, &firstClick);
    QCOMPARE(freshButtons.count(), 1);

    quint16 expectX = 0;
    quint16 expectY = 0;
    QVERIFY(lanrdp::ScreenRenderer::mapToNormalized(QPoint(10, 10), QSize(1920, 1080),
                                                    QSize(960, 540), &expectX, &expectY));
    QCOMPARE(freshButtons.at(0).at(2).value<quint16>(), expectX);
    QCOMPARE(freshButtons.at(0).at(3).value<quint16>(), expectY);
    QVERIFY2(!(expectX == 0 && expectY == 0), "首次点击不该退化成左上角 (0,0)");
}

// 提示层只属于「这一格有远端」的情况。本机预览格显示的是自己的桌面，
// 画「点击画面，即可操作对方电脑」是纯误导。
void TestRenderer::hintOnlyForCellsWithRemote()
{
    lanrdp::ScreenRenderer renderer;
    QVERIFY2(!renderer.inputHintVisible(), "没有画面时不该有提示");
    QVERIFY2(renderer.inputHintEnabled(), "默认应当认为本格可操作（控制格）");

    QImage image(64, 48, QImage::Format_ARGB32);
    image.fill(Qt::green);
    renderer.setFrame(image);
    QVERIFY2(renderer.inputHintVisible(), "有画面且键鼠未启用时应当提示");

    renderer.setInputHintEnabled(false);
    QVERIFY(!renderer.inputHintEnabled());
    QVERIFY2(!renderer.inputHintVisible(), "本格没有远端时不得提示");

    renderer.setInputHintEnabled(true);
    QVERIFY2(renderer.inputHintVisible(), "恢复后提示应当回来");

    renderer.setInputEnabled(true);
    QVERIFY2(!renderer.inputHintVisible(), "键鼠已接管时提示让位");
}

// 点提示 = 请求打开总开关。但「这一格没有远端」时不能发这个请求 ——
// 上层会据此打开**全窗口**的键鼠采集，交给一个没人接的渲染器，本机点击原地消失。
void TestRenderer::pressOnHintRequestsEnableOnlyWhenHintShown()
{
    lanrdp::ScreenRenderer renderer;
    QImage image(1920, 1080, QImage::Format_ARGB32);
    image.fill(Qt::darkGray);
    renderer.setFrame(image);
    renderer.resize(960, 540);

    QSignalSpy requests(&renderer, &lanrdp::ScreenRenderer::inputEnableRequested);

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

    renderer.setInputHintEnabled(false);
    QCoreApplication::sendEvent(&renderer, &press);
    QCOMPARE(requests.count(), 0);

    renderer.setInputHintEnabled(true);
    QCoreApplication::sendEvent(&renderer, &press);
    QCOMPARE(requests.count(), 1);
}

QTEST_MAIN(TestRenderer)
#include "test_renderer.moc"
