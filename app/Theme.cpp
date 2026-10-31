#include "app/Theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace lanrdp::theme {

namespace {

// 色值表。这里的每一个数都受 tests/test_theme.cpp 的对比度校验约束 ——
// 调色时先看测试，别只凭肉眼。
const Colors kColors{
    QColor(0x1E, 0x1E, 0x24),   // windowBg
    QColor(0x25, 0x25, 0x2D),   // surface
    QColor(0x2E, 0x2E, 0x38),   // surfaceHover
    QColor(0x34, 0x34, 0x3F),   // surfaceActive
    QColor(0x18, 0x18, 0x1C),   // canvas
    QColor(0x38, 0x38, 0x44),   // border
    QColor(0x4A, 0x4A, 0x58),   // borderStrong
    QColor(0xE8, 0xE8, 0xEE),   // textPrimary
    QColor(0xA8, 0xA8, 0xB6),   // textSecondary
    QColor(0x6E, 0x6E, 0x7C),   // textMuted
    QColor(0x25, 0x63, 0xEB),   // accent
    QColor(0x3B, 0x82, 0xF6),   // accentHover
    QColor(0xFF, 0xFF, 0xFF),   // onAccent
    QColor(0xF0, 0x64, 0x5A),   // danger
    QColor(0x4A, 0xDE, 0x80),   // success
    QColor(0xF0, 0xB8, 0x4A),   // warning
    QColor(0x1E, 0x1E, 0x25),   // overlayBar
    QColor(0x24, 0x28, 0x33),   // overlayBarSelected
};

// 强调色用 #2563EB 而不是更亮的蓝：白字压在 #3B82F6 上只有 3.7:1，
// 够不上正文的 4.5:1；#2563EB 是 5.2:1。更亮的那个留给悬停。
QPalette darkPalette(const Colors &c)
{
    QPalette p;

    p.setColor(QPalette::Window, c.windowBg);
    p.setColor(QPalette::WindowText, c.textPrimary);
    p.setColor(QPalette::Base, c.surface);
    p.setColor(QPalette::AlternateBase, c.surfaceHover);
    p.setColor(QPalette::ToolTipBase, c.surfaceHover);
    p.setColor(QPalette::ToolTipText, c.textPrimary);
    p.setColor(QPalette::Text, c.textPrimary);
    p.setColor(QPalette::PlaceholderText, c.textMuted);
    p.setColor(QPalette::Button, c.surface);
    p.setColor(QPalette::ButtonText, c.textPrimary);
    p.setColor(QPalette::BrightText, c.danger);
    p.setColor(QPalette::Link, c.accentHover);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, c.onAccent);

    // 立体感的四个色阶：Fusion 画边框 / 凹槽 / 阴影时用。
    p.setColor(QPalette::Light, c.surfaceHover);
    p.setColor(QPalette::Midlight, c.surface);
    p.setColor(QPalette::Mid, c.border);
    p.setColor(QPalette::Dark, c.canvas);
    p.setColor(QPalette::Shadow, Qt::black);

    // 禁用态：只降文字，不动底色 —— 底色一变，禁用控件看起来像「另一个控件」。
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.textMuted);
    p.setColor(QPalette::Disabled, QPalette::Text, c.textMuted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.textMuted);
    p.setColor(QPalette::Disabled, QPalette::Base, c.windowBg);
    p.setColor(QPalette::Disabled, QPalette::Button, c.windowBg);

    return p;
}

} // namespace

const Colors &colors()
{
    return kColors;
}

void apply(QApplication &app)
{
    // 幂等：换样式会把所有控件重新 polish 一遍，重复调用纯属白折腾。
    static bool applied = false;
    if (applied)
        return;
    applied = true;

    // ① 先告诉 Qt「本应用要深色」。
    //    Windows 会据此把窗口标题栏也刷成深色 —— 浅色标题栏压在深色内容上，
    //    是整扇窗最扎眼的一条白边。必须在设 palette 之前：反过来的话，
    //    系统主题会把刚设好的 palette 又刷一遍。
    if (QStyleHints *hints = QGuiApplication::styleHints())
        hints->setColorScheme(Qt::ColorScheme::Dark);

    // ② Fusion。
    //    Windows 原生样式（windowsvista）有相当多控件不吃 palette ——
    //    只改 palette 会出现「输入框已经是深色、下拉箭头还是浅色」这种半截效果。
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);

    app.setPalette(darkPalette(kColors));
    app.setStyleSheet(applicationStyleSheet());
}

QString applicationStyleSheet()
{
    const Colors &c = colors();

    // 分四段拼：QString::arg 的占位符只有 %1..%99，一段塞十几个颜色容易数错，
    // 而且 %10 与 %1 的解析边界本身就不该去赌。每段都控制在 9 个以内。

    // ① 窗口与提示
    QString ss = QStringLiteral(R"(
QMainWindow, QDialog { background: %1; }
QToolTip {
    background: %2;
    color: %3;
    border: 1px solid %4;
    padding: 4px 6px;
}
)").arg(c.windowBg.name(), c.surfaceHover.name(), c.textPrimary.name(), c.border.name());

    // ② 菜单栏与菜单。
    //    这两样是全局 QSS 里最值钱的部分 —— Fusion 的默认菜单在深色 palette 下
    //    仍然是浅灰底，跟内容区对不上。
    ss += QStringLiteral(R"(
QMenuBar {
    background: %1;
    color: %2;
    border-bottom: 1px solid %3;
    padding: 2px 4px;
}
QMenuBar::item {
    background: transparent;
    padding: 4px 10px;
    border-radius: 4px;
}
QMenuBar::item:selected { background: %4; }
QMenuBar::item:pressed  { background: %5; }

QMenu {
    background: %6;
    color: %2;
    border: 1px solid %3;
    padding: 4px;
}
QMenu::item {
    padding: 6px 24px 6px 24px;
    border-radius: 4px;
}
QMenu::item:selected { background: %4; }
QMenu::item:disabled { color: %7; }
QMenu::separator {
    height: 1px;
    background: %3;
    margin: 4px 8px;
}
)").arg(c.windowBg.name(), c.textPrimary.name(), c.border.name(),
       c.surfaceHover.name(), c.surfaceActive.name(), c.surface.name(),
       c.textMuted.name());

    // ③ 状态栏与输入控件。
    //    状态栏比窗口底再深一档，让「内容区 / 状态区」在视觉上分层，
    //    而不是靠一条描边硬切。
    ss += QStringLiteral(R"(
QStatusBar {
    background: %1;
    color: %2;
    border-top: 1px solid %3;
}
QStatusBar::item { border: none; }

QLineEdit, QSpinBox, QComboBox {
    background: %4;
    color: %5;
    border: 1px solid %3;
    border-radius: 5px;
    padding: 5px 8px;
    selection-background-color: %6;
    selection-color: %7;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: %6; }
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled { color: %8; }
)").arg(c.windowBg.darker(115).name(), c.textSecondary.name(), c.border.name(),
       c.surface.name(), c.textPrimary.name(), c.accent.name(),
       c.onAccent.name(), c.textMuted.name());

    // ④ 按钮（含对话框里的默认按钮）
    ss += QStringLiteral(R"(
QPushButton {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: 5px;
    padding: 5px 16px;
    min-height: 20px;
}
QPushButton:hover   { background: %4; border-color: %5; }
QPushButton:pressed { background: %6; }
QPushButton:default {
    background: %7;
    border-color: %7;
    color: %8;
}
QPushButton:default:hover { background: %9; border-color: %9; }
)").arg(c.surface.name(), c.textPrimary.name(), c.border.name(),
       c.surfaceHover.name(), c.borderStrong.name(), c.surfaceActive.name(),
       c.accent.name(), c.onAccent.name(), c.accentHover.name());

    // ⑤ 禁用态、复选框、进度条、四处对话框提示（口令报错 / 连接报错 / 表单说明 / 防火墙）。
    //    QCheckBox::indicator / QSpinBox 的上下按钮**故意不写** —— 交给 Fusion
    //    按 palette 上色。手写 indicator 想加勾号还得配图片资源，反而更差。
    //
    //    ⚠️ 报错行必须在这里有规则，且颜色取 danger：没有规则时它落回默认调色板的
    //    正文色，看上去是「提示」而不是「错误」，而单测只断言可见性与文案，查不出来。
    ss += QStringLiteral(R"(
QPushButton:disabled { color: %1; background: %2; border-color: %3; }

QCheckBox { color: %4; spacing: 8px; }
QCheckBox:disabled { color: %1; }

QProgressBar {
    background: %5;
    border: 1px solid %3;
    border-radius: 4px;
    text-align: center;
    color: %4;
}
QProgressBar::chunk { background: %6; border-radius: 3px; }

QLabel#passwordErrorHint { color: %7; }
QLabel#connectErrorHint { color: %7; }
QLabel#formHint { color: %1; }
QLabel#firewallHint { color: %8; }
)").arg(c.textMuted.name(), c.surface.name(), c.border.name(),
       c.textPrimary.name(), c.surfaceActive.name(), c.accent.name(),
       c.danger.name(), c.textSecondary.name());

    // ⑥ 口令框右侧的「睁眼 / 划眼」开关。
    //    它不是主次操作（和【确定】不在一个量级），所以不留边框、不留底色，
    //    别在表单里再抢一块视觉重量。
    //    ⚠️ 这里不写 color：那颗按钮显示的是 QIcon，QSS 的 color 管不到图标，
    //    颜色在创建图标时就定了。留下的 padding 只为让它离边框有点余量。
    ss += QStringLiteral(R"(
QToolButton#passwordToggleButton {
    background: transparent;
    border: none;
    padding: 0 4px;
}
)");

    return ss;
}

QString overlayBarStyleSheet()
{
    const Colors &c = colors();

    // 条压在画面上，底色比控件面再深一点、文字更亮 —— 这样无论下面是白色网页
    // 还是深色壁纸，条都能靠自身对比度站住，而不是靠「比背景浅一点」。
    QString ss = QStringLiteral(R"(
QToolButton {
    min-height: 18px;
    padding: 0 8px;
    border: 1px solid transparent;
    border-radius: 3px;
    color: %1;
    background: transparent;
}
QToolButton:hover {
    border-color: %2;
    background: %3;
    color: %4;
}
QToolButton:pressed { background: %5; }
QToolButton:checked {
    background: %6;
    border-color: %7;
    color: %4;
}
QToolButton:disabled { color: %8; }

QLabel#overlayTitle { color: %1; font-weight: bold; }

/* 操作权状态是一颗**不可点**的图标：它没有动作，就不该有按下去的反馈，
   否则会诱人去点。hover / pressed / checked 全部按成透明（ID 选择器比
   上面那条 QToolButton:hover 更具体，能压住它）。
   注意：QSS 只认块注释，行首双斜杠会让解析器把从这里到下一个右花括号的
   整段（也就是下面这三条规则）一起丢掉，等于没写。 */
QToolButton#tokenStatusButton:hover,
QToolButton#tokenStatusButton:pressed,
QToolButton#tokenStatusButton:checked {
    border-color: transparent;
    background: transparent;
}
)").arg(c.textPrimary.name(),      // 1 常规文字
        c.borderStrong.name(),     // 2 hover 描边
        c.surfaceActive.name(),    // 3 hover 底
        c.textPrimary.name(),      // 4 悬停 / 选中文字
        c.surface.name(),          // 5 按下底
        c.accent.name(),           // 6 选中底
        c.accentHover.name(),      // 7 选中描边
        c.textMuted.name());       // 8 禁用

    // 【断开】是危险动作，用的是告警色 —— 但它是**图标按钮**，颜色落在图形上
    // （`RemoteView` 建按钮时取 `colors().danger`），样式表里的 color 管不到 QIcon，
    // 所以这里不再单开一段规则。

    return ss;
}

QString emptyStateStyleSheet()
{
    const Colors &c = colors();

    // 字号用 px（QSS 的 px 是设备无关像素，4K@150% 会自动放大）。
    return QStringLiteral(R"(
QLabel#emptyTitle {
    color: %1;
    font-size: 15px;
}
QLabel#emptySubtitle {
    color: %2;
    font-size: 12px;
}

QPushButton#emptyPrimary {
    background: %3;
    border: 1px solid %3;
    border-radius: 5px;
    color: %4;
    font-size: 12px;
    padding: 7px 18px;
    min-height: 18px;
}
QPushButton#emptyPrimary:hover {
    background: %5;
    border-color: %5;
}
QPushButton#emptyPrimary:pressed { background: %3; }

QPushButton#emptySecondary {
    background: transparent;
    border: 1px solid %6;
    border-radius: 5px;
    color: %2;
    font-size: 12px;
    padding: 7px 16px;
    min-height: 18px;
}
QPushButton#emptySecondary:hover {
    background: %7;
    border-color: %6;
    color: %1;
}
)").arg(c.textPrimary.name(),      // 1 标题
        c.textSecondary.name(),    // 2 副标题 / 次按钮文字
        c.accent.name(),           // 3 主按钮底
        c.onAccent.name(),         // 4 主按钮文字
        c.accentHover.name(),      // 5 主按钮悬停
        c.borderStrong.name(),     // 6 次按钮描边
        c.surfaceHover.name());    // 7 次按钮悬停底
}

} // namespace lanrdp::theme
