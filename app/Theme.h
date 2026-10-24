#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace lanrdp::theme {

// 深色主题的色板 —— 全仓界面色值的**唯一出处**。
//
// 改配色只改这一处：各文件的 QSS 与自绘代码都从 colors() 取值，不再写字面色值。
// 从前条底色 / 条内按钮 / 危险按钮 / 对话框提示色散落在三个文件里，改一处忘一处。
//
// ⚠️ 这里存的是**不透明**色。需要半透明的地方（悬浮条要透出一点画面）在绘制处
// 单独 setAlpha —— 对比度校验必须按不透明值算，否则量出来的数是假的。
struct Colors {
    QColor windowBg;           // 主窗口 / 对话框底
    QColor surface;            // 输入框 / 菜单 / 状态栏这类「面」
    QColor surfaceHover;       // 悬停
    QColor surfaceActive;      // 按下 / 选中
    QColor canvas;             // 画面底（空态与 letterbox 的留白）
    QColor border;             // 常规描边
    QColor borderStrong;       // 需要看清的描边（输入框聚焦前的边）
    QColor textPrimary;        // 正文
    QColor textSecondary;      // 说明文字
    QColor textMuted;          // 禁用 / 占位符（只保证 3:1，不用于正文）
    QColor accent;             // 强调色：主按钮底、选中线、焦点框
    QColor accentHover;        // 强调色的悬停
    QColor onAccent;           // 压在强调色上的文字
    QColor danger;             // 危险动作（断开 / 口令警告）
    QColor success;            // 已连接状态点
    QColor warning;            // 重连中 / 最后一帧
    QColor overlayBar;         // 悬浮操作条底
    QColor overlayBarSelected; // 选中那一格的条底
};

const Colors &colors();

// 给应用装上深色外观：系统深色方案 + Fusion 样式 + 深色 palette + 全局 QSS。
//
// 顺序不可反：先告诉 Qt「本应用要深色」（窗口标题栏跟着变深），再覆盖 palette，
// 最后上 QSS。反过来 palette 会被系统主题重新刷掉。
// 幂等 —— 重复调用只生效一次（换样式会重新 polish 所有控件，没必要折腾两遍）。
void apply(QApplication &app);

// 全局 QSS：菜单栏 / 菜单 / 状态栏 / 输入控件 / 按钮 / 进度条。
//
// 刻意**不碰** QCheckBox::indicator、QSpinBox 的上下按钮 —— 那些交给 Fusion 画。
// Fusion 会按 palette 的 Highlight/Text 上色，比手写 QSS 更准（手写 indicator
// 想加勾号还得配图片资源）。
QString applicationStyleSheet();

// 悬浮操作条的内务样式。
//
// 条高只有 30 逻辑像素，按钮必须收紧，所以不能沿用全局那套 padding；
// 但配色必须跟主题一致，否则深色化之后条里还留着一撮亮色文字。
QString overlayBarStyleSheet();

// 空态层的样式（未选模式时盖在画面上的那一屏）。
//
// 单独一份是因为它要用「主按钮 / 次按钮」两档：全局的 QPushButton 规则是
// 对话框口径（灰底描边），拿来当主按钮会跟次按钮长得一样。
QString emptyStateStyleSheet();

} // namespace lanrdp::theme
