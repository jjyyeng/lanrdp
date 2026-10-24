#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

namespace lanrdp::glyph {

// 界面里的矢量小图标。
//
// 每条路径都画在 **24×24 的设计网格**上，出图时才等比缩放到目标尺寸 ——
// 所以同一份路径既当 18px 的按钮图标，也当 68px 的空态大图，不必为高 DPI
// 准备多套位图，也不必往 qrc 里塞 svg（那还得带上 qsvgicon 插件，缺了只是
// 静默不出图）。
//
// 颜色在**创建时**固定：项目没有运行时换主题（theme::apply() 只在启动时调一次），
// 做成随 palette 重算反而多一层要维护的间接。禁用态由引擎自己降透明度。
enum class Shape {
    Monitor,       // 显示器轮廓
    MonitorOut,    // 屏幕 + 指出去的箭头（连对方）
    MonitorIn,     // 屏幕 + 指进来的箭头（让对方连进来）
    Maximize,      // 一对朝画布角的箭头（占满这一格）
    Restore,       // 同一对箭头转成朝内收（退出占满）
    Lock,          // 闭合的挂锁（收回操作权 = 锁上本机键鼠）
    Unlock,        // 锁梁弹开（恢复操作 / 让出操作权 = 放开键鼠控制）
    Pointer,       // 实心指针（操作权已获得）
    PointerIdle,   // 空心指针（还没拿到操作权）
    SendFile,      // 托盘 + 朝上的箭头（把文件送出去）
    Eye,           // 眼睛（看画面 / 看明文口令）
    EyeOff,        // 眼睛 + 一道斜杠（遮起来：隐藏口令）
    Person,        // 单个人形（已连几个控制端）
    Listen,        // 实心播放三角（开始监听）
    StopListen,    // 实心方块（停止监听）
    Reconnect,     // 环形箭头（重连）
    Disconnect,    // 圆圈加一道斜杠（断开这一条连接）
    CloseCell,     // 叉（关掉这一格）
    NewConnection, // 屏幕 + 加号（再连一台）
};

QIcon   icon(Shape shape, const QColor &color);
QPixmap pixmap(Shape shape, int size, qreal devicePixelRatio, const QColor &color);

} // namespace lanrdp::glyph
