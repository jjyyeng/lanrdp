#include "net/TcpRtt.h"

#ifdef Q_OS_WIN
// NTDDI_VERSION 必须在任何 SDK 头之前定：SIO_TCP_INFO 与 TCP_INFO_v0 都挂在
// 「NTDDI_VERSION >= WIN10_RS2」这个条件下，而 MinGW 默认不定义它 ——
// 不显式抬高的话，#include <mstcpip.h> 直接少了这两样东西，症状是编译期
// 「SIO_TCP_INFO 未定义」，看不出是版本门槛在作怪。
#  ifndef NTDDI_VERSION
#    define NTDDI_VERSION 0x0A000006   // WIN10_RS5
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <windows.h>
#  include <mstcpip.h>
#endif

namespace lanrdp {

int queryTcpRttMs(qintptr socketDescriptor)
{
    if (socketDescriptor < 0)
        return -1;

#ifdef Q_OS_WIN
    TCP_INFO_v0 info{};
    // 这个 ioctl 的**输入**是「要哪个版本的结构体」（0 = TCP_INFO_v0）。
    // 传空输入会被判 WSAEINVAL(10022) 而不是崩掉 —— 一个很容易被当成
    // 「系统不支持」的假象。
    DWORD version  = 0;
    DWORD returned = 0;
    if (::WSAIoctl(static_cast<SOCKET>(socketDescriptor), SIO_TCP_INFO, &version,
                   sizeof(version), &info, sizeof(info), &returned, nullptr, nullptr)
        != 0) {
        return -1;
    }
    if (returned < sizeof(info))
        return -1;

    // 内核给微秒。四舍五入到毫秒：0.05 ms 报 0 不叫「没测到」，报 1 才是谎报。
    return static_cast<int>((info.RttUs + 500u) / 1000u);
#else
    Q_UNUSED(socketDescriptor);
    return -1;
#endif
}

} // namespace lanrdp
