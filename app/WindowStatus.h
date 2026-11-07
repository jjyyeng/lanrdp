#pragma once

#include <QString>

namespace lanrdp {

// 窗口标题与任务栏角标的**判据**都在这里，UI 只负责把结果贴上去。
// 单独抽出来的理由：判据是纯函数，可以脱离窗口单测 —— 而「本机此刻是否
// 正在被人看」这件事不允许靠肉眼看窗口去验证。

// 未监听、也没有控制端时的标题基准。
QString baseWindowTitle();

// 从 "ip:port"（或 "[::1]:port"）里取出地址部分；没有端口时原样返回。
QString peerAddress(const QString &peer);

// 窗口标题。目标读者是**被控端的机主**：他最小化窗口、被别的窗口挡住时，
// 只看任务栏也要能一眼判断有没有人在看自己的屏幕。
QString hostWindowTitle(bool listening, int clientCount, const QString &firstPeer);

// 任务栏角标要显示的数字；0 = 不加角标。
int hostBadgeCount(bool listening, int clientCount);

// 最小化时该不该把窗口收进通知区域。
//
// 三条同时成立才收：设置里打开了、托盘图标真的挂上了、窗口确实最小化了。
// 中间那条是保命的 —— 没有通知区域却把窗口藏起来，它就没有任何地方能被
// 叫回来，用户只能去任务管理器结束进程。
bool hideToTrayOnMinimize(bool trayResident, bool trayVisible, bool minimized);

// 点窗口关闭按钮时到底发生了什么。
enum class CloseAction {
    Quit,        // 真的退出程序
    HideToTray,  // 收进通知区域，继续在后台服务
};

// 关闭窗口的判据。
//
// 分档的理由是「关闭」在这里有两种完全不同的含义：
//   · 正在待客 / 正在被看 → 关窗只是把脸收起来，同事还在连着，不能断
//   · 什么都没干 → 关窗就是退出，藏起来只会变成「以为退了其实还在跑」
// 没有通知区域时一律退出：藏起来等于窗口凭空消失，比退出更糟。
CloseAction closeAction(bool trayResident, bool trayVisible, bool hostBusy);

} // namespace lanrdp
