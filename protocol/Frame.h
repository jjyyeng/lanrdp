#pragma once

#include <QByteArray>
#include <QMetaType>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace lanrdp {

// 帧 Magic 'LR'。作为 2 字节标签，按“先 'L' 后 'R'”写在线上，
// 因此 0x4C52 以 big-endian 形式落到字节流：0x4C('L'), 0x52('R')。
constexpr std::uint16_t kMagic    = 0x4C52;
// 协议版本。
// **v2 = 挑战-响应握手**（服务端先发 Challenge，客户端回 HMAC 响应）。
// **v3 = CLIPBOARD_TEXT 去掉 4 字节 `seq` 头部**（载荷就是裸 UTF-8 文本）。
// **v4 = 引入音频两帧**（AUDIO_CONFIG 17 / AUDIO_DATA 18，单向系统声音）。
// v1 的「客户端先发 sha256 摘要」流程已废弃且被拒（tryParse 对不上版本直接 Invalid）。
// 单二进制双角色、两端同时更新 ⇒ 无兼容负担（同心跳帧：本就要求同版本）。
constexpr std::uint8_t  kVersion  = 5;

// 帧头固定 8 字节：
// Magic[2]='LR' | Version[1] | Type[1] | PayloadLen[4, little-endian]
constexpr std::size_t kHeaderSize = 8;

// PayloadLen 的硬上限。
//
// 为什么必须有：帧头在鉴权**之前**就被解析，PayloadLen 又是 32 位，
// 未认证的对端只要发送一个声明 0xFFFFFFFF 的头再缓慢滴流，接收缓冲就会
// 无上限增长直到进程 OOM；而 tryParse 对荒唐长度只会返回 Incomplete
// （永远等下去），状态不会自愈。
//
// 取值依据：需容纳最大 4K H.264 关键帧（实测远小于 4 MiB），32 MiB 留足余量。
// 注意：这不是「业务帧」的上限，而是「流已损坏」的判定门槛。
//
// ⚠️ 裸帧（未走 H.264 的回退路径）离这条线很近：4K BGRA = 3840×2160×4 + 8
// 已达 33,177,608 字节，只差 1.1%；5K/8K 必然超限。超限的帧**发不出去**
// （`serialize()` 拒发、`Session::sendFrame` 丢帧并警告），不会悄悄截断成
// 半帧 —— 截断只会让对端判流损坏然后断连，那种失败现场无从诊断。
constexpr std::uint32_t kMaxPayloadLen = 32u * 1024u * 1024u;

// 帧类型（值一经确定不再改变）
//
// 注意必须用 enum class：非作用域枚举会把 InputEvent / FileHeader 等枚举量
// 注入 lanrdp 命名空间，与同名的消息结构体（如 InputEvent）撞名。
enum class FrameType : std::uint8_t
{
    HandshakeReq    = 1,
    HandshakeOk     = 2,
    HandshakeDeny   = 3,
    ScreenFrameRaw  = 4,
    InputEvent      = 5,
    ScreenFrameH264 = 6,
    FileHeader      = 7,
    FileChunk       = 8,
    FileEnd         = 9,
    TokenReq        = 10,
    TokenGrant      = 11,
    TokenRelease    = 12,
    ClipboardText   = 13,
    Heartbeat       = 14, // 单向保活：各自定时器发送，收到**不回帧**（回复会乒乓成风暴）
    // 挑战-响应握手：服务端先发 Challenge（32 B nonce），客户端回 Response
    // （HMAC-SHA256(口令, nonce)）。HandshakeReq 保留为「客户端声明 + 发起」帧，
    // 载荷为空 —— 摘要不再上线，被动嗅探者拿不到可重放的东西。
    HandshakeChallenge = 15,
    HandshakeResponse  = 16,
    // 音频（v4）：配置帧先于数据帧到达（TCP 有序）。
    AudioConfig        = 17, // 载体格式（采样率 / 声道 / 位深 / 块长）
    AudioData          = 18, // 时间戳 + S16LE 交织 PCM
    // 多屏（v5）：LIST 是状态、SELECT 是请求，语义见 protocol/ScreenControl.h。
    ScreenList         = 19, // 屏列表 + 当前在用哪块（被控端 → 控制端，唯一权威）
    ScreenSelect       = 20, // 请求切到第几块（控制端 → 被控端）
};

// 从字节流前端解析一帧的结果
enum class ParseStatus
{
    Ok,         // 解析成功，消费掉一整帧
    Incomplete, // 数据不足，需等待更多字节
    Invalid,    // Magic / Version 非法（流已损坏）
};

struct Frame
{
    std::uint8_t type    = 0;
    std::uint8_t version = kVersion;
    QByteArray   payload;

    // 序列化为字节流（头 + payload）。
    QByteArray serialize() const;

    // 便捷解析：仅当 data 前端恰有一整帧且合法时返回该帧，否则 nullopt。
    static std::optional<Frame> parse(const QByteArray &data);

    // 增量解析（供 Session 流式分帧使用）：
    // Ok -> *out 填充该帧，*consumed = 该帧总字节数
    // Incomplete -> 目标不变（等待更多字节）
    // Invalid -> 目标不变（调用方应断开）
    //
    // `offset` 允许从 data 的中间开始解析（默认 0）。
    // Session 靠它在缓冲里推进下标而不拷贝前缀 —— 否则每帧都要 mid() 一份，
    // 小帧洪峰下退化为 O(n²)。*consumed 仍报「该帧自身总字节数」，不含 offset。
    static ParseStatus tryParse(const QByteArray &data, Frame *out, qsizetype *consumed,
                                qsizetype offset = 0);
};

} // namespace lanrdp

Q_DECLARE_METATYPE(lanrdp::Frame)
