#pragma once

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QString>

namespace lanrdp {

// 控制端连不上时给用户的「下一步」。
//
// socket->errorString() 是英文，且只描述现象。用户看到 "Connection refused"
// 并不知道该去确认对方开没开程序、改端口，还是去放行防火墙 —— 而这三件事的
// 处置完全不同。
//
// 返回空串 = 这类错误没有确定的建议。未知错误只说现象，别猜原因：猜错方向
// 比不给建议更糟（用户会去改一件根本不相关的事）。
//
// ⚠️ 分流的关键是「包到没到」：
//   · ConnectionRefused —— 包到了、被拒了 ⇒ 网络是通的，问题在对面（没监听 / 端口错）
//   · 超时 / 网络不可达 —— 包被丢了 ⇒ 才该提防火墙与网段
// 这两种说反，用户就会去查一个不存在的问题。
// ⚠️ 这几句会出现在状态栏 / 格的说明里，所以跟着界面语言走。自由函数没有
// QObject 上下文，只能显式给上下文名；而且字面量要**直接写进 translate()**，
// 用 lambda / 辅助函数转发会让 lupdate 抽不到（英文界面里会静默留中文）。
inline QString connectionHint(QAbstractSocket::SocketError error)
{
    switch (error) {
    case QAbstractSocket::ConnectionRefusedError:
        return QCoreApplication::translate("lanrdp",
            "对方未监听该端口：确认对方已开始监听、两边端口一致。");

    case QAbstractSocket::HostNotFoundError:
        return QCoreApplication::translate("lanrdp",
            "地址无法解析：检查 IP 或主机名是否写错。");

    case QAbstractSocket::SocketTimeoutError:
    case QAbstractSocket::NetworkError:
        return QCoreApplication::translate("lanrdp",
            "对方无回应：检查防火墙是否放行本程序、两台是否同网段。");

    case QAbstractSocket::RemoteHostClosedError:
        return QCoreApplication::translate("lanrdp",
            "对方断开了：可能已停止监听，或连接数已满。");

    default:
        return QString();
    }
}

} // namespace lanrdp
