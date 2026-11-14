#pragma once

#include <QRandomGenerator>
#include <QString>

namespace lanrdp {

// 口令的硬性要求（被控端与控制端的对话框用同一条规则）。
//
// 这是**唯一判定口**：被控端三处（对话框 accept / startHost / --host 解析）与
// 控制端的【新连接…】都用它。散在几处写「长度 ≥ 6」的话，改一处漏一处 ——
// 而漏掉的那一处就是一条绕过口令校验的口子。
//
// ⚠️ 控制端**只在对话框**用它：`--connect` 与 `startConnect()` 是自动化入口，
// 口令要能跟着被测端走，不能被 UI 规则绑住。
//
// 只收 ASCII 可打印字符（0x21–0x7E），**不含空格**：空格在命令行里要引号、
// 手输又看不见，是纯粹的错误来源。所以长度直接按字符数算（= 字节数，无歧义）。
inline constexpr int kMinPasswordLength = 6;

inline bool isValidPassword(const QString &password)
{
    if (password.size() < kMinPasswordLength)
        return false;

    for (const QChar ch : password) {
        const ushort code = ch.unicode();
        if (code < 0x21 || code > 0x7E)
            return false;
    }
    return true;
}

// 生成一个合规口令（【开始监听…】里的【生成】按钮用）。
//
// 字符池刻意排除 0 / O / 1 / l / I —— 它们在等宽字体里几乎同形。用户生成完
// 要把口令念给旁边的人，抄错一位是最常见的故障，而症状看起来像「口令本身错了」。
// 默认 12 位（约 70 bit 熵）：够长，也仍然念得完。
//
// 随机源用 QRandomGenerator::system()，与握手的 nonce 同一个源，不新引入随机性假设。
// bounded() 取下标有可忽略的模偏差（池子 69 个字符），对实际强度没有影响。
//
// 唯一承诺：**产出的串一定过 isValidPassword**，所以请求长度低于下限时按
// 下限生成，而不是返回一个注定被拒的短串。
inline QString generatePassword(int length = 12)
{
    static const char kPool[] =
        "abcdefghijkmnopqrstuvwxyz"   // 去掉 l
        "ABCDEFGHJKLMNPQRSTUVWXYZ"    // 去掉 I、O
        "23456789"                    // 去掉 0、1
        "!@#$%^&*-_=+";
    constexpr int kPoolSize = int(sizeof(kPool) - 1);

    if (length < kMinPasswordLength)
        length = kMinPasswordLength;

    QString out;
    out.reserve(length);
    for (int i = 0; i < length; ++i) {
        const quint32 idx = QRandomGenerator::system()->bounded(kPoolSize);
        out.append(QLatin1Char(kPool[idx]));
    }
    return out;
}

} // namespace lanrdp
