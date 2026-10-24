#pragma once

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

} // namespace lanrdp
