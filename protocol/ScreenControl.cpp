#include "protocol/ScreenControl.h"

#include "protocol/LE.h"

namespace lanrdp {

namespace {

bool isValidScreenInfo(const ScreenInfo &screen)
{
    if (screen.width == 0 || screen.width > kMaxScreenDimension)
        return false;
    if (screen.height == 0 || screen.height > kMaxScreenDimension)
        return false;
    if (screen.x < -kMaxScreenOrigin || screen.x > kMaxScreenOrigin)
        return false;
    if (screen.y < -kMaxScreenOrigin || screen.y > kMaxScreenOrigin)
        return false;
    return true;
}

} // namespace

const ScreenInfo *ScreenList::currentScreen() const
{
    if (current >= screens.size())
        return nullptr;
    return &screens.at(current);
}

bool isValidScreenList(const ScreenList &list)
{
    if (list.screens.isEmpty() || list.screens.size() > kMaxScreens)
        return false;
    if (list.current >= list.screens.size())
        return false;

    // 「恰好一个主屏」：一个都没有等于界面上无法标出主屏，两个以上说明发送端
    // 自己就算不清（真实 Windows 环境不可能出现，出现了就是数据有问题）。
    int primaries = 0;
    for (const ScreenInfo &screen : list.screens) {
        if (!isValidScreenInfo(screen))
            return false;
        if (screen.primary)
            ++primaries;
    }
    return primaries == 1;
}

QByteArray encodeScreenList(const ScreenList &list)
{
    if (!isValidScreenList(list))
        return QByteArray();

    QByteArray out;
    out.reserve(kScreenListHeaderSize + kScreenEntrySize * list.screens.size());
    le::appendU8(out, kScreenControlVersion);
    le::appendU8(out, static_cast<quint8>(list.screens.size()));
    le::appendU8(out, list.current);

    for (const ScreenInfo &screen : list.screens) {
        le::appendI32(out, screen.x);
        le::appendI32(out, screen.y);
        le::appendU32(out, screen.width);
        le::appendU32(out, screen.height);
        le::appendU8(out, screen.primary ? 1 : 0);
    }
    return out;
}

std::optional<ScreenList> decodeScreenList(const QByteArray &payload)
{
    if (payload.size() < kScreenListHeaderSize)
        return std::nullopt;
    if (le::readU8(payload, 0) != kScreenControlVersion)
        return std::nullopt;

    const int count = le::readU8(payload, 1);
    if (count <= 0 || count > kMaxScreens)
        return std::nullopt;

    // 长度必须精确匹配：多出来的字节说明两端对布局的认知已经不一致了。
    // ⚠️ 先比长度再分配 —— 拿不可信的 count 去 reserve 之前必须确认载荷真有那么多。
    if (payload.size() != kScreenListHeaderSize + kScreenEntrySize * count)
        return std::nullopt;

    ScreenList list;
    list.screens.reserve(count);
    list.current = le::readU8(payload, 2);

    qsizetype offset = kScreenListHeaderSize;
    for (int i = 0; i < count; ++i) {
        ScreenInfo screen;
        screen.x       = le::readI32(payload, offset);
        screen.y       = le::readI32(payload, offset + 4);
        screen.width   = le::readU32(payload, offset + 8);
        screen.height  = le::readU32(payload, offset + 12);
        screen.primary = le::readU8(payload, offset + 16) != 0;
        list.screens.append(screen);
        offset += kScreenEntrySize;
    }

    // 结构性的（数量 / 主屏唯一 / current 范围 / 每块几何）一并在这里挡住。
    if (!isValidScreenList(list))
        return std::nullopt;
    return list;
}

QByteArray encodeScreenSelect(quint8 index)
{
    // 控制端界面只可能点列表里有的项，越界说明调用方算错了 —— 直接拒发，
    // 让错误停在本地而不是变成一次「被对端静默纠正的请求」。
    if (index >= kMaxScreens)
        return QByteArray();

    QByteArray out;
    out.reserve(kScreenSelectSize);
    le::appendU8(out, kScreenControlVersion);
    le::appendU8(out, index);
    return out;
}

std::optional<quint8> decodeScreenSelect(const QByteArray &payload)
{
    if (payload.size() != kScreenSelectSize)
        return std::nullopt;
    if (le::readU8(payload, 0) != kScreenControlVersion)
        return std::nullopt;

    const quint8 index = le::readU8(payload, 1);
    if (index >= kMaxScreens)
        return std::nullopt;
    return index;
}

} // namespace lanrdp
