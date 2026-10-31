#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QVector>

#include <optional>

namespace lanrdp {

// 多屏两帧的线上格式（协议 v5，帧类型 19 / 20）。
//
// 分工是这一项的核心，别混：
// · `SCREEN_LIST` 是**状态** —— 屏列表 + 当前正在编哪一块，由被控端广播给所有控制端。
//   它是「现在推的是哪块屏」的**唯一权威**，控制端只显示最近收到的那一份。
// · `SCREEN_SELECT` 是**请求** —— 控制端说「我想看第几块」。校验失败时被控端
//   不改状态，只回一份新 `SCREEN_LIST`；控制端界面因此自动回到真实状态，
//   不需要错误码（与「拒 TokenReq 必须回一帧 TokenRelease」同一思路）。
//
// 为什么列表里每次都带完整几何：index 只是**本进程内的枚举序**，插拔 / 改缩放后
// 同一块屏的 index 会变。几何是控制端唯一能自证「这是我以为的那块屏」的信息。
// 代价是每屏 17 字节，一台机器几块屏完全无所谓。

// 这一层自己的载荷版本（与帧头 Version 无关）。将来加字段靠它区分。
constexpr quint8 kScreenControlVersion = 1;

// 一块屏最多 16 块 —— 上限只服务于「count 是不可信输入」，不是产品约束。
constexpr quint8 kMaxScreens = 16;

// 单边像素上限。8K 是 7680，留了四倍余量；超了基本可以断定是伪造载荷。
constexpr quint32 kMaxScreenDimension = 32768;

// 坐标绝对值上限。虚拟桌面允许负数坐标（屏在主屏左侧 / 上方），
// 但不允许「一百万」这种 —— 归一化坐标会因此算到一个荒谬的点上。
constexpr qint32 kMaxScreenOrigin = 65536;

// 每块屏：x:i32 + y:i32 + w:u32 + h:u32 + primary:u8 = 17 B
constexpr qsizetype kScreenEntrySize = 17;
// 列表头：ver:u8 + count:u8 + current:u8 = 3 B
constexpr qsizetype kScreenListHeaderSize = 3;
// 选择请求：ver:u8 + index:u8 = 2 B
constexpr qsizetype kScreenSelectSize = 2;

struct ScreenInfo
{
    qint32  x      = 0;   // 物理像素，屏幕坐标系（主屏左侧 / 上方时为负）
    qint32  y      = 0;
    quint32 width  = 0;
    quint32 height = 0;
    bool    primary = false;
};

struct ScreenList
{
    // 下标即 index（线上不单独传序号）：这样「列表第 N 项」与
    // `SCREEN_SELECT` 里的 index 天然是同一个东西，不存在两套编号对不上的可能。
    QVector<ScreenInfo> screens;
    quint8              current = 0;

    // 当前那块屏；列表为空或 current 越界时返回 nullptr。
    const ScreenInfo *currentScreen() const;
};

// 合法性：1..kMaxScreens 块、每块尺寸 1..kMaxScreenDimension、坐标不越界、
// primary **恰好一个**、current 必须指向已有项。
bool isValidScreenList(const ScreenList &list);

// 非法输入返回**空数组**（= 拒发），与 `Frame::serialize` 同一口径：
// 宁可什么都不发，也不要发出一份自相矛盾的状态。
QByteArray encodeScreenList(const ScreenList &list);

// 长度 / 版本 / 各字段范围任一不符即 `nullopt`（不「近似解析」）。
std::optional<ScreenList> decodeScreenList(const QByteArray &payload);

QByteArray               encodeScreenSelect(quint8 index);
std::optional<quint8>    decodeScreenSelect(const QByteArray &payload);

} // namespace lanrdp

// 信号参数用它（跨线程排队投递时 Qt 要求类型已注册）。
Q_DECLARE_METATYPE(lanrdp::ScreenList)
