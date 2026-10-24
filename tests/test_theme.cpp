#include <QtTest/QtTest>

#include <QApplication>
#include <QImage>
#include <QPalette>
#include <QStyle>

#include <cmath>

#include "app/Glyph.h"
#include "app/Theme.h"

// 界面外观的守护测试。
//
// 这一层测不了「好不好看」—— 那没法断言。能断言的是三件事：
//   ① 主题确实是深色（浅底 + 浅字在长时间盯画面之后刺眼，4K 屏上还会让窗口边缘泛光）；
//   ② 每一对「文字 / 底色」的对比度达到 WCAG AA 的 4.5:1 —— 凭肉眼调色，
//      迟早会调出一处看不清的；
//   ③ apply() 真的把样式换成了 Fusion —— 否则 palette 在 Windows 原生样式下
//      有一半控件不认（菜单、下拉箭头、微调按钮仍走系统画法），深色化只做了一半。
namespace {

// WCAG 2.1 相对亮度：sRGB 分量先线性化，再按 0.2126 / 0.7152 / 0.0722 加权。
double relativeLuminance(const QColor &c)
{
    const auto linear = [](int channel) {
        const double v = channel / 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(c.red()) + 0.7152 * linear(c.green()) + 0.0722 * linear(c.blue());
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

QString describe(const QColor &c)
{
    return c.name(QColor::HexRgb);
}

} // namespace

class TestTheme : public QObject
{
    Q_OBJECT

private slots:
    void paletteIsDark();
    void textSurvivesOnEverySurface();
    void mutedTextIsStillLegibleWhenDisabled();
    void applyInstallsFusionAndPalette();
    void styleSheetsUseThePalette();

    // 矢量图标：任何尺寸 / 任何 DPI 都要出图，而且图上得真有墨
    void glyphsDrawAtEverySize();
    // 禁用态的图标必须明显变淡，否则「现在不能点」只能靠文字判断
    void disabledGlyphIsFaded();
    // 图形的纵向重心必须落在画布中线上：按钮里图标和文字并排，偏一格就错半行
    void glyphShapesAreVerticallyCentered();
    // 【占满】那颗按钮不带文字，两个状态只能靠图形区分 —— 画成一样就等于没状态
    void maximizeAndRestoreDiffer();
    // 操作权那几张图同样是「不带文字的按钮」：锁定 / 放行、握着 / 空手都要分得开
    void operationGlyphsDiffer();
    // 【开始 / 停止监听】三角 ⇄ 方块，同样是「不带文字」的一对
    void listenAndStopDiffer();
    // 口令框那颗开关：睁开 ⇄ 划掉，也是「不带文字」的一对
    void eyeOnOffDiffer();
};

// 底色全部落在暗部、文字全部落在亮部 —— 这是「深色主题」最低限度的定义。
// 阈值取的是相对亮度而不是「看着像深色」：人眼对 0.05 与 0.10 的区别不可靠。
void TestTheme::paletteIsDark()
{
    const auto &c = lanrdp::theme::colors();

    const struct {
        const char *name;
        QColor      color;
        double      limit;   // 相对亮度上限
    } darkSurfaces[] = {
        {"窗口底 windowBg", c.windowBg, 0.10},
        {"控件面 surface", c.surface, 0.10},
        {"画面底 canvas", c.canvas, 0.05},
        {"悬浮条 overlayBar", c.overlayBar, 0.10},
        {"选中条 overlayBarSelected", c.overlayBarSelected, 0.12},
    };

    for (const auto &s : darkSurfaces) {
        QVERIFY2(relativeLuminance(s.color) < s.limit,
                 qPrintable(QStringLiteral("%1 = %2 的相对亮度 %3 未低于 %4，撑不起「深色」")
                                .arg(QString::fromUtf8(s.name), describe(s.color))
                                .arg(relativeLuminance(s.color), 0, 'f', 4)
                                .arg(s.limit)));
    }

    QVERIFY2(relativeLuminance(c.textPrimary) > 0.60,
             qPrintable(QStringLiteral("正文色 %1 太暗，压在深底上看不清").arg(describe(c.textPrimary))));
}

// 文字与底色的组合是穷举的 —— 少列一对，就等于给那一处留了个看不见的漏洞。
void TestTheme::textSurvivesOnEverySurface()
{
    const auto &c = lanrdp::theme::colors();

    const struct {
        const char *where;
        QColor      bg;
        QColor      fg;
    } pairs[] = {
        {"正文 / 窗口底", c.windowBg, c.textPrimary},
        {"说明文字 / 窗口底", c.windowBg, c.textSecondary},
        {"正文 / 控件面", c.surface, c.textPrimary},
        {"说明文字 / 控件面", c.surface, c.textSecondary},
        {"正文 / 悬浮条", c.overlayBar, c.textPrimary},
        {"说明文字 / 悬浮条", c.overlayBar, c.textSecondary},
        {"正文 / 选中条", c.overlayBarSelected, c.textPrimary},
        {"按钮文字 / 主按钮底", c.accent, c.onAccent},
        {"危险动作 / 悬浮条", c.overlayBar, c.danger},
        {"正文 / 按下态底", c.surfaceActive, c.textPrimary},
    };

    for (const auto &p : pairs) {
        const double ratio = contrastRatio(p.fg, p.bg);
        QVERIFY2(ratio >= 4.5,
                 qPrintable(QStringLiteral("%1：%2 压在 %3 上只有 %4:1，够不上 4.5:1")
                                .arg(QString::fromUtf8(p.where), describe(p.fg), describe(p.bg))
                                .arg(ratio, 0, 'f', 2)));
    }
}

// 禁用态可以降到 3:1（WCAG 对「非活动控件」放宽），但不能更低 ——
// 用户仍需要读出「这个按钮写的是什么，只是现在点不了」。
void TestTheme::mutedTextIsStillLegibleWhenDisabled()
{
    const auto &c = lanrdp::theme::colors();

    const double onWindow = contrastRatio(c.textMuted, c.windowBg);
    const double onSurface = contrastRatio(c.textMuted, c.surface);
    QVERIFY2(onWindow >= 3.0,
             qPrintable(QStringLiteral("禁用文字 / 窗口底只有 %1:1").arg(onWindow, 0, 'f', 2)));
    QVERIFY2(onSurface >= 3.0,
             qPrintable(QStringLiteral("禁用文字 / 控件面只有 %1:1").arg(onSurface, 0, 'f', 2)));
}

void TestTheme::applyInstallsFusionAndPalette()
{
    lanrdp::theme::apply(*qApp);

    const auto &c = lanrdp::theme::colors();
    QCOMPARE(qApp->palette().color(QPalette::Window), c.windowBg);
    QCOMPARE(qApp->palette().color(QPalette::Base), c.surface);
    QCOMPARE(qApp->palette().color(QPalette::WindowText), c.textPrimary);
    QCOMPARE(qApp->palette().color(QPalette::Highlight), c.accent);

    QVERIFY2(!qApp->styleSheet().isEmpty(), "apply() 应当装上全局 QSS");

    // ⚠️ 装了 QSS 之后，qApp->style() 返回的是 Qt 的包装器 QStyleSheetStyle，
    // 看不到底下的真实样式（objectName 也是空的）。临时清空 QSS 让它退回底层，
    // 确认确实是 Fusion，再装回去 —— 这是从外部唯一能观察到「Fusion 生效」的办法。
    qApp->setStyleSheet(QString());
    const QByteArray baseStyle = qApp->style()->metaObject()->className();
    qApp->setStyleSheet(lanrdp::theme::applicationStyleSheet());
    QVERIFY2(baseStyle == "QFusionStyle",
             qPrintable(QStringLiteral("底层样式是 %1，不是 Fusion")
                            .arg(QString::fromLatin1(baseStyle))));

    // 幂等：重复调用不该再把样式换一遍（换样式会重新 polish 所有控件）。
    QStyle *before = qApp->style();
    lanrdp::theme::apply(*qApp);
    QCOMPARE(qApp->style(), before);
}

// 光有色板还不够 —— QSS 若还是旧色值，深色化就只剩一半。
// 这条断言挡住「改了 colors() 却忘了同步 QSS」这种最容易发生的回归。
void TestTheme::styleSheetsUseThePalette()
{
    const auto &c = lanrdp::theme::colors();
    const QString app = lanrdp::theme::applicationStyleSheet();
    const QString bar = lanrdp::theme::overlayBarStyleSheet();

    QVERIFY2(!app.isEmpty(), "全局 QSS 不应为空");
    QVERIFY2(!bar.isEmpty(), "悬浮条 QSS 不应为空");

    // 关键选择器必须在场：少了它们，对应控件就退回系统画法。
    const char *required[] = {
        "QMenuBar", "QMenu", "QStatusBar", "QLineEdit", "QSpinBox",
        "QPushButton", "QProgressBar", "QCheckBox",
    };
    for (const char *selector : required) {
        QVERIFY2(app.contains(QLatin1String(selector)),
                 qPrintable(QStringLiteral("全局 QSS 缺少 %1 的规则").arg(QLatin1String(selector))));
    }

    // 占位符必须全部被替换掉。写成 %a / %10 这类 QString::arg 不认的形式时，
    // 它会原样留在样式表里 —— 那条规则静默失效，界面看起来「没生效但也没报错」。
    QVERIFY2(!app.contains(QLatin1Char('%')), "全局 QSS 里有没被替换的占位符");
    QVERIFY2(!bar.contains(QLatin1Char('%')), "条内 QSS 里有没被替换的占位符");

    // 深色主题的底色必须真的出现在样式表里。
    QVERIFY2(app.contains(c.windowBg.name()), "全局 QSS 未使用 windowBg");
    QVERIFY2(app.contains(c.surface.name()), "全局 QSS 未使用 surface");
    QVERIFY2(bar.contains(c.textPrimary.name()), "条内 QSS 未使用正文色");
    // 危险色（【断开】）现在落在**图标**上而不是样式表里 —— QSS 的 color 管不到
    // QIcon，那条规则只会让人误以为颜色是从这儿来的。守它的是
    // `test_mainwindow_ui::disconnectButtonStartsDisabled`（比对图标实色）。
}

// 「矢量」不是嘴上说的：同一份路径要在 12px 到 68px、1x 到 2x 之间都画得出来。
//
// 顺带守住更难发现的一种失败 —— **出了图但整张全透明**（路径画到矩形外、
// 缩放系数算成 0）。`isNull()` 对这种情况是查不出来的，只能看像素。
void TestTheme::glyphsDrawAtEverySize()
{
    const QColor ink = lanrdp::theme::colors().textPrimary;

    const auto shapes = { lanrdp::glyph::Shape::Monitor,
                          lanrdp::glyph::Shape::MonitorOut,
                          lanrdp::glyph::Shape::MonitorIn,
                          lanrdp::glyph::Shape::Maximize,
                          lanrdp::glyph::Shape::Restore,
                          lanrdp::glyph::Shape::Lock,
                          lanrdp::glyph::Shape::Unlock,
                          lanrdp::glyph::Shape::Pointer,
                          lanrdp::glyph::Shape::PointerIdle,
                          lanrdp::glyph::Shape::SendFile,
                          lanrdp::glyph::Shape::Eye,
                          lanrdp::glyph::Shape::EyeOff,
                          lanrdp::glyph::Shape::Person,
                          lanrdp::glyph::Shape::Listen,
                          lanrdp::glyph::Shape::StopListen,
                          lanrdp::glyph::Shape::Reconnect,
                          lanrdp::glyph::Shape::Disconnect,
                          lanrdp::glyph::Shape::CloseCell,
                          lanrdp::glyph::Shape::NewConnection };
    const char *names[]  = { "Monitor", "MonitorOut", "MonitorIn", "Maximize", "Restore",
                             "Lock",    "Unlock",     "Pointer",    "PointerIdle",
                             "SendFile", "Eye", "EyeOff", "Person",
                             "Listen", "StopListen", "Reconnect",
                             "Disconnect", "CloseCell", "NewConnection" };
    const int   sizes[]  = { 12, 16, 18, 24, 68 };
    const qreal ratios[] = { 1.0, 1.5, 2.0 };

    int i = 0;
    for (auto shape : shapes) {
        for (int size : sizes) {
            for (qreal dpr : ratios) {
                const QPixmap pm = lanrdp::glyph::pixmap(shape, size, dpr, ink);
                const QString what = QStringLiteral("%1 size=%2 dpr=%3")
                                         .arg(QString::fromLatin1(names[i]))
                                         .arg(size).arg(dpr);

                QVERIFY2(!pm.isNull(), qPrintable(what + QStringLiteral("：出了空图")));
                // 位图必须是「逻辑尺寸 × 设备像素比」—— 少数一倍就是高 DPI 下发虚的根。
                QCOMPARE(pm.width(), qRound(size * dpr));
                QCOMPARE(pm.height(), qRound(size * dpr));
                QCOMPARE(pm.devicePixelRatio(), dpr);

                const QImage img = pm.toImage();
                int painted = 0;
                for (int y = 0; y < img.height(); ++y) {
                    for (int x = 0; x < img.width(); ++x) {
                        if (qAlpha(img.pixel(x, y)) > 0)
                            ++painted;
                    }
                }
                QVERIFY2(painted > 0, qPrintable(what + QStringLiteral("：整张图一个像素都没画上")));
            }
        }
        ++i;
    }
}

// 图标在禁用按钮上必须明显变淡 —— 否则「这颗现在点不了」只能靠读文字猜。
void TestTheme::disabledGlyphIsFaded()
{
    const QColor ink = lanrdp::theme::colors().textPrimary;
    const QIcon  ic  = lanrdp::glyph::icon(lanrdp::glyph::Shape::Monitor, ink);

    const auto peakAlpha = [](const QImage &img) {
        int peak = 0;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x)
                peak = qMax(peak, qAlpha(img.pixel(x, y)));
        }
        return peak;
    };

    const int on  = peakAlpha(ic.pixmap(QSize(32, 32), 1.0, QIcon::Normal).toImage());
    const int off = peakAlpha(ic.pixmap(QSize(32, 32), 1.0, QIcon::Disabled).toImage());

    QVERIFY2(on > 0, "正常态图标是空的，这条断言就没有意义了");
    QVERIFY2(off > 0, "禁用态不该整张消失 —— 那等于没有图标");
    QVERIFY2(off < on * 0.7,
             qPrintable(QStringLiteral("禁用态峰值 alpha %1 对正常态 %2 没有明显变淡")
                            .arg(off).arg(on)));
}

// 按钮里的图标是**整块**居中摆放的（Qt 不会替我们去对齐图形本身），
// 所以「图形在 24 格画布里是否居中」直接决定它和文字看起来齐不齐。
// 这里量的是真正被涂上的像素的上下边界，不是几何框 —— 支架、描边伸出去多少都算数。
void TestTheme::glyphShapesAreVerticallyCentered()
{
    const QColor ink = lanrdp::theme::colors().textPrimary;
    constexpr int kSize = 64;
    constexpr qreal kSizeF = 64.0;

    const auto shapes = { lanrdp::glyph::Shape::Monitor,
                          lanrdp::glyph::Shape::MonitorOut,
                          lanrdp::glyph::Shape::MonitorIn,
                          lanrdp::glyph::Shape::Maximize,
                          lanrdp::glyph::Shape::Restore,
                          lanrdp::glyph::Shape::Lock,
                          lanrdp::glyph::Shape::Unlock,
                          lanrdp::glyph::Shape::Pointer,
                          lanrdp::glyph::Shape::PointerIdle,
                          lanrdp::glyph::Shape::SendFile,
                          lanrdp::glyph::Shape::Eye,
                          lanrdp::glyph::Shape::EyeOff,
                          lanrdp::glyph::Shape::Person,
                          lanrdp::glyph::Shape::Listen,
                          lanrdp::glyph::Shape::StopListen,
                          lanrdp::glyph::Shape::Reconnect,
                          lanrdp::glyph::Shape::Disconnect,
                          lanrdp::glyph::Shape::CloseCell,
                          lanrdp::glyph::Shape::NewConnection };
    const char *names[] = { "Monitor", "MonitorOut", "MonitorIn", "Maximize", "Restore",
                            "Lock",    "Unlock",     "Pointer",    "PointerIdle",
                            "SendFile", "Eye", "EyeOff", "Person",
                            "Listen", "StopListen", "Reconnect",
                            "Disconnect", "CloseCell", "NewConnection" };

    int i = 0;
    for (auto shape : shapes) {
        const QImage img = lanrdp::glyph::pixmap(shape, kSize, 1.0, ink).toImage();

        int top = -1;
        int bottom = -1;
        for (int y = 0; y < img.height(); ++y) {
            bool rowHasInk = false;
            for (int x = 0; x < img.width(); ++x) {
                if (qAlpha(img.pixel(x, y)) > 40) {   // 忽略抗锯齿的极淡边缘
                    rowHasInk = true;
                    break;
                }
            }
            if (rowHasInk) {
                if (top < 0)
                    top = y;
                bottom = y;
            }
        }

        QVERIFY2(top >= 0, qPrintable(QStringLiteral("%1：整张图没画出东西")
                                          .arg(QString::fromLatin1(names[i]))));

        const qreal center = (top + bottom + 1) / 2.0;
        const qreal offset = center - kSizeF / 2.0;
        QVERIFY2(qAbs(offset) <= 1.5,
                 qPrintable(QStringLiteral("%1：图形纵向重心在 %2，画布中线在 %3，偏了 %4 px"
                                               "（和文字并排时会错开半个字）")
                                .arg(QString::fromLatin1(names[i]))
                                .arg(center, 0, 'f', 1)
                                .arg(kSizeF / 2.0, 0, 'f', 1)
                                .arg(offset, 0, 'f', 1)));
        ++i;
    }
}

// 【占满】那颗按钮上**没有文字**（半格宽的条塞不下五个带字的按钮），
// 「现在是占满着还是没占满」全靠图形区分。两个形状要是画成一样，用户就没法判断了。
void TestTheme::maximizeAndRestoreDiffer()
{
    const QColor ink = lanrdp::theme::colors().textPrimary;
    constexpr int kSize = 32;

    const QImage maximize =
        lanrdp::glyph::pixmap(lanrdp::glyph::Shape::Maximize, kSize, 1.0, ink).toImage();
    const QImage restore =
        lanrdp::glyph::pixmap(lanrdp::glyph::Shape::Restore, kSize, 1.0, ink).toImage();

    QVERIFY2(!maximize.isNull() && !restore.isNull(), "两枚图标都得画得出来");
    QVERIFY2(maximize != restore,
             "占满与还原画成了同一个形状 —— 不带文字时用户看不出当前是哪一态");
}

// 【开始 / 停止监听】也是双态图标按钮：三角 ⇄ 方块。画成一样就等于没有状态。
void TestTheme::listenAndStopDiffer()
{
    const QColor ink = lanrdp::theme::colors().textPrimary;
    constexpr int kSize = 32;

    const QImage listen =
        lanrdp::glyph::pixmap(lanrdp::glyph::Shape::Listen, kSize, 1.0, ink).toImage();
    const QImage stop =
        lanrdp::glyph::pixmap(lanrdp::glyph::Shape::StopListen, kSize, 1.0, ink).toImage();

    QVERIFY2(!listen.isNull() && !stop.isNull(), "两枚图标都得画得出来");
    QVERIFY2(listen != stop,
             "开始监听与停止监听画成了同一个形状 —— 不带文字时用户看不出当前是哪一态");
}

// 操作权那三颗按钮同样没有文字：
// · 被控端【收回操作权 / 恢复操作】= 闭锁 / 开锁；
// · 控制端的操作权状态 = 实心指针（握着）/ 空心指针（空手）。
// 每一对都必须画得不一样，否则条上就是两个一样的小方块。
void TestTheme::operationGlyphsDiffer()
{
    const QColor ink = lanrdp::theme::colors().textPrimary;
    constexpr int kSize = 32;

    const auto render = [&ink](lanrdp::glyph::Shape s) {
        return lanrdp::glyph::pixmap(s, kSize, 1.0, ink).toImage();
    };

    const QImage lock      = render(lanrdp::glyph::Shape::Lock);
    const QImage unlock    = render(lanrdp::glyph::Shape::Unlock);
    const QImage held      = render(lanrdp::glyph::Shape::Pointer);
    const QImage idle      = render(lanrdp::glyph::Shape::PointerIdle);

    QVERIFY2(!lock.isNull() && !unlock.isNull() && !held.isNull() && !idle.isNull(),
             "有人没画出东西 —— 空图标在按钮上就是一块空白");
    QVERIFY2(lock != unlock, "收回与恢复画成了同一个形状");
    QVERIFY2(held != idle, "「已获得」与「没拿到」画成了同一个形状");

    // 空心的那张不能空到整张消失：它得能看出是个指针，只是没填充。
    const auto inkPixels = [](const QImage &img) {
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (qAlpha(img.pixel(x, y)) > 40)
                    ++n;
        return n;
    };
    QVERIFY2(inkPixels(idle) > 0, "空心指针必须仍有轮廓，否则等于没有状态图标");
    QVERIFY2(inkPixels(held) > inkPixels(idle),
             "实心态应当比空心态更饱满 —— 这是两态唯一的视觉差别");
}

// 口令框那颗开关同样不带文字：睁开 / 划掉两张图必须一眼分得开。
// 而且划掉那张不能只剩一道斜杠 —— 眼睛要是被斜杠吃掉，它就变成「禁止」了。
void TestTheme::eyeOnOffDiffer()
{
    const QColor ink = lanrdp::theme::colors().textPrimary;
    constexpr int kSize = 18;   // 按钮上的实际尺寸

    const auto render = [&ink](lanrdp::glyph::Shape s) {
        return lanrdp::glyph::pixmap(s, kSize, 1.0, ink).toImage();
    };
    const auto inkPixels = [](const QImage &img) {
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (qAlpha(img.pixel(x, y)) > 40)
                    ++n;
        return n;
    };

    const QImage open = render(lanrdp::glyph::Shape::Eye);
    const QImage shut = render(lanrdp::glyph::Shape::EyeOff);

    QVERIFY2(!open.isNull() && !shut.isNull(), "眼睛有一态没画出东西");
    QVERIFY2(open != shut, "睁开与划掉画成了同一张图，用户看不出现在是哪一态");

    // 划掉的那张应当比睁开的多一点墨（多了斜杠），但不能多太多 ——
    // 一旦超过两成，说明两条笔画已经糊在一起，眼睛反倒看不清了。
    const int openInk = inkPixels(open);
    const int shutInk = inkPixels(shut);
    QVERIFY2(openInk > 0, "睁开的眼睛是空的");
    QVERIFY2(shutInk > openInk,
             "划掉那一态没有多出斜杠的墨迹 —— 两道弧叠在一起，等于没有状态");
    QVERIFY2(shutInk < openInk * 2,
             qPrintable(QStringLiteral("划掉那一态多了太多墨（%1 → %2）：斜杠和眼睛糊在一起了")
                            .arg(openInk).arg(shutInk)));
}

QTEST_MAIN(TestTheme)
#include "test_theme.moc"
