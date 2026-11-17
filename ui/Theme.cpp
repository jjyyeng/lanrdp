#include "ui/Theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace lanrdp::theme {

namespace {

// 两套色板。这里的每一个数都受 tests/test_theme.cpp 的对比度校验约束 ——
// 调色时先看测试，别只凭肉眼。
//
// 深色：底色落在暗部、文字落在亮部。
const Colors kDark{
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

// 浅色：同样的角色，方向反过来。几处不是「简单取反」的地方：
// · canvas 比窗口底更深一档（画面留白要在白底上看得出来），对应深色里它更深；
// · accentHover 比 accent **更深** —— 浅色主题的 hover 惯例是压暗，而浅色底上
//   白字压更亮的蓝会掉到 3:1 以下；
// · danger / warning 都取深一号的色阶：它们是压在浅底上的文字（口令报错、
//   明文链路警告），用深色主题那种亮红亮黄在 #FFFFFF 上只剩 2~3:1。
const Colors kLight{
    QColor(0xF5, 0xF6, 0xF8),   // windowBg
    QColor(0xFF, 0xFF, 0xFF),   // surface
    QColor(0xEC, 0xEF, 0xF4),   // surfaceHover
    QColor(0xE1, 0xE5, 0xEC),   // surfaceActive
    QColor(0xE6, 0xE9, 0xEE),   // canvas
    QColor(0xD3, 0xD8, 0xE0),   // border
    QColor(0xAE, 0xB6, 0xC4),   // borderStrong
    QColor(0x1C, 0x1F, 0x26),   // textPrimary
    QColor(0x4C, 0x54, 0x62),   // textSecondary
    QColor(0x76, 0x7E, 0x8C),   // textMuted
    QColor(0x25, 0x63, 0xEB),   // accent
    QColor(0x1D, 0x4E, 0xD8),   // accentHover
    QColor(0xFF, 0xFF, 0xFF),   // onAccent
    QColor(0xB3, 0x26, 0x1E),   // danger
    QColor(0x1A, 0x9E, 0x58),   // success
    QColor(0x8A, 0x53, 0x00),   // warning
    QColor(0xEF, 0xF2, 0xF7),   // overlayBar
    QColor(0xE3, 0xEB, 0xF9),   // overlayBarSelected
};

Mode   g_mode = Mode::Dark;
Colors g_active = kDark;   // 切模式时**整体改写**，指针不变（见头文件）

// 强调色用 #2563EB 而不是更亮的蓝：白字压在 #3B82F6 上只有 3.7:1，
// 够不上正文的 4.5:1；#2563EB 是 5.2:1。深色主题里更亮的那个留给悬停。
QPalette paletteFor(const Colors &c, Mode mode)
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
    // 浅色主题的「暗部」只能到描边为止 —— 拿 canvas 当 Dark 会让凹陷效果
    // 变成一圈灰糊（浅色下 canvas 与 surface 只差一档）。
    if (mode == Mode::Light) {
        p.setColor(QPalette::Light, c.surface);
        p.setColor(QPalette::Midlight, c.surfaceHover);
        p.setColor(QPalette::Mid, c.border);
        p.setColor(QPalette::Dark, c.borderStrong);
        p.setColor(QPalette::Shadow, QColor(0x8A, 0x91, 0x9E));
    } else {
        p.setColor(QPalette::Light, c.surfaceHover);
        p.setColor(QPalette::Midlight, c.surface);
        p.setColor(QPalette::Mid, c.border);
        p.setColor(QPalette::Dark, c.canvas);
        p.setColor(QPalette::Shadow, Qt::black);
    }

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
    return g_active;
}

const Colors &colorsFor(Mode mode)
{
    return mode == Mode::Light ? kLight : kDark;
}

Mode mode()
{
    return g_mode;
}

QString modeKey(Mode mode)
{
    return mode == Mode::Light ? QStringLiteral("light") : QStringLiteral("dark");
}

Mode modeFromKey(const QString &key, Mode fallback)
{
    const QString trimmed = key.trimmed().toLower();
    if (trimmed == QLatin1String("light"))
        return Mode::Light;
    if (trimmed == QStringLiteral("dark"))
        return Mode::Dark;
    return fallback;
}

void setMode(Mode mode)
{
    g_mode   = mode;
    g_active = colorsFor(mode);
}

void apply(QApplication &app)
{
    // ① 先告诉 Qt 本应用要深色还是浅色。
    //    Windows 会据此把窗口标题栏也刷成同色系 —— 深色标题栏压在浅色内容上
    //    （或者反过来）是整扇窗最扎眼的一条边。必须在设 palette 之前：
    //    反过来的话，系统主题会把刚设好的 palette 又刷一遍。
    if (QStyleHints *hints = QGuiApplication::styleHints())
        hints->setColorScheme(g_mode == Mode::Light ? Qt::ColorScheme::Light
                                                    : Qt::ColorScheme::Dark);

    // ② Fusion。**只装一次**：换样式会把所有控件重新 polish 一遍，
    //    而且外面拿到的 QStyle* 会失效（测试正是靠这一点判断「没白折腾」）。
    //
    //    Windows 原生样式（windowsvista）有相当多控件不吃 palette ——
    //    只改 palette 会出现「输入框已经是深色、下拉箭头还是浅色」这种半截效果。
    static bool styleInstalled = false;
    if (!styleInstalled) {
        if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
            app.setStyle(fusion);
        styleInstalled = true;
    }

    app.setPalette(paletteFor(g_active, g_mode));
    app.setStyleSheet(applicationStyleSheet());
}

int overlayBarAlpha(bool selected)
{
    // 92% 不透明（深色）：留一点画面透出来，让人看得出「条浮在画面上」；
    // 再低就压不住白色远端窗口，文字会跟着背景一起发飘。
    // 浅色主题几乎盖实（98%）：半透明白压在深色画面上只会糊成一层脏灰。
    if (g_mode == Mode::Light)
        return selected ? 252 : 250;
    return selected ? 242 : 235;
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
    //    而不是靠一条描边硬切。（浅色主题下 darker() 也是「更深一档」，
    //    方向天然正确，不必分模式。）
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

    // ③b 自动发现列表（【新连接…】顶部那块）。
    //     外观与输入框同族：它在语义上就是「拿来填进上面那几个框」的东西，
    //     长成另一种样子会让人以为它是另一类操作。
    //     ⚠️ 选中态必须写成 `::item:selected` 并**显式给背景**：只要给 `::item`
    //     设了 padding / border-radius，这一格就改由样式表画，`selection-*`
    //     那两个属性只剩文字那半边管用 —— 字变白、底不变。深色主题的底本来就深，
    //     白字看着没事；浅色主题的底是白的，白字压上去等于消失。
    ss += QStringLiteral(R"(
QListWidget#peerList {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: 5px;
    padding: 2px;
    selection-background-color: %4;
    selection-color: %5;
}
QListWidget#peerList::item { padding: 4px 6px; border-radius: 4px; }
QListWidget#peerList::item:hover { background: %6; }
QListWidget#peerList::item:selected { background: %4; color: %5; }
)").arg(c.surface.name(), c.textPrimary.name(), c.border.name(),
       c.accent.name(), c.onAccent.name(), c.surfaceHover.name());

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
QLabel#insecureHint { color: %9; }
)").arg(c.textMuted.name(), c.surface.name(), c.border.name(),
       c.textPrimary.name(), c.surfaceActive.name(), c.accent.name(),
       c.danger.name(), c.textSecondary.name(), c.warning.name());

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

    // 条压在画面上，底色与文字必须靠自身对比度站住，而不是靠「比背景浅一点」。
    // 深色主题是深底浅字、浅色主题是浅底深字，两者的取值都在色板里。
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
    color: %1;
}
QToolButton:pressed { background: %4; }
QToolButton:disabled { color: %5; }

/* 【发送文件】待发时右侧多出来的那颗小三角。箭头本身由 style 按 palette 画
   （QSS 的 menu-arrow 只认位图，我们没有 qrc），这里只管「让它自成一块」：
   一道分隔线就够让人看出按钮右边还有一块可点，不必去赌箭头在当前主题下的深浅。 */
QToolButton::menu-button {
    border: none;
    border-left: 1px solid %2;
    width: 13px;
}
/* 那一块是**贴着按钮右边缘**画的，会压掉常规的右 padding（8px < 14px），
   于是分隔线正好压在图标右边沿上。挂了小三角时把右侧让出来，图标随之左移，
   两者各占一块、互不重叠。宽度必须跟着一起加 —— 只挪位置不加宽的话，
   内容区被压窄，图标会被挤到左边沿。 */
QToolButton[menuArrow="true"] {
    padding-right: 22px;
}

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
)").arg(c.textPrimary.name(),      // 1 常规文字 / 悬停文字
        c.borderStrong.name(),     // 2 hover 描边
        c.surfaceActive.name(),    // 3 hover 底
        c.surface.name(),          // 4 按下底
        c.textMuted.name());       // 5 禁用

    // 选中态的底是强调色 —— 压在上面的文字必须取 onAccent，**不能**跟着常规文字走：
    // 浅色主题的正文色是近黑的，压在 #2563EB 上只有 3:1。深色主题里 onAccent 是
    // 纯白，比正文色（#E8E8EE）还稳一点，所以两套都取它。
    ss += QStringLiteral(R"(
QToolButton:checked {
    background: %1;
    border-color: %2;
    color: %3;
}
)").arg(c.accent.name(), c.accentHover.name(), c.onAccent.name());

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
