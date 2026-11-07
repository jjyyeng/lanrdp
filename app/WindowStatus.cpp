#include "app/WindowStatus.h"

namespace lanrdp {

QString baseWindowTitle()
{
    return QStringLiteral("LAN RDP");
}

QString peerAddress(const QString &peer)
{
    const QString trimmed = peer.trimmed();
    if (trimmed.isEmpty())
        return QString();

    // 「地址 + 端口」按最后一个冒号切；IPv6 是 "[::1]:9002" 这种带方括号的写法，
    // 切完再把括号剥掉。没有冒号（调用的地方给了裸地址）原样返回。
    const int colon = trimmed.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0)
        return trimmed;

    QString host = trimmed.left(colon);
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')) && host.size() > 2)
        host = host.mid(1, host.size() - 2);
    return host;
}

int hostBadgeCount(bool listening, int clientCount)
{
    // 没在监听 ⇒ 不可能有人连着（断开监听会连带断开全部控制端）。
    // 监听中但空着也不该有角标 —— 角标的语义是「有人正在看」，不是「我在待客」。
    if (!listening || clientCount <= 0)
        return 0;
    return clientCount;
}

QString hostWindowTitle(bool listening, int clientCount, const QString &firstPeer)
{
    if (!listening)
        return baseWindowTitle();

    if (clientCount <= 0)
        return QStringLiteral("%1 — 等待连接").arg(baseWindowTitle());

    if (clientCount == 1) {
        // 只有一个连接时点名道姓，比「1 个连接」有用得多；拿不到地址就退回泛称。
        const QString who = peerAddress(firstPeer);
        return who.isEmpty()
                   ? QStringLiteral("%1 — 正在被对方控制").arg(baseWindowTitle())
                   : QStringLiteral("%1 — 正在被 %2 控制").arg(baseWindowTitle(), who);
    }

    return QStringLiteral("%1 — %2 个连接正在查看本机").arg(baseWindowTitle()).arg(clientCount);
}

bool hideToTrayOnMinimize(bool trayResident, bool trayVisible, bool minimized)
{
    return trayResident && trayVisible && minimized;
}

CloseAction closeAction(bool trayResident, bool trayVisible, bool hostBusy)
{
    // 空闲时藏起来 = 「我以为退了，其实还有一个实例在后台等你连」。
    // 那正是这个功能最该避免的误会，所以只有真的在服务时才藏。
    if (!trayResident || !trayVisible || !hostBusy)
        return CloseAction::Quit;
    return CloseAction::HideToTray;
}

} // namespace lanrdp
