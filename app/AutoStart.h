#pragma once

#include <QString>

namespace lanrdp {

// 「开机后自动开始等待连接」在系统里的落点：当前用户 Run 键下的一个值。
//
// 只写 HKCU，不碰 HKLM、不建计划任务、不需要管理员权限 —— 这个程序的交付形态
// 就是「拷个目录就能跑」，一条自启项不该把普通用户挡在外面。
//
// 键路径可注入：单测必须写进 `HKCU\Software\lanrdp-test\Run`，否则跑一次测试就往
// 用户真实的自启列表里塞一条，那是「跑完没人发现、重启才看见」的脏副作用。
//
// ⚠️ Run 键里存的是**绝对路径**。程序目录一挪，旧值就指向一个不存在的文件，
// 症状是「明明开着却再也没自启过」，而且系统不会给任何提示 —— 所以每次启动
// 都要用 refresh() 把路径校正到当前 exe。
class AutoStart
{
public:
    explicit AutoStart(const QString &runKeyPath = defaultRunKeyPath());

    static QString defaultRunKeyPath();

    // 单测 / 临时配置用的键路径。
    //
    // 调用方（MainWindow）按「配置文件的位置」选键：配置文件不是默认那份时，
    // 自启项也不该落进用户真实的自启列表 —— 否则跑一次 UI 测试就往开机项里
    // 塞一条测试 exe，且没人会注意到。
    static QString testRunKeyPath();

    // Run 键里有没有一条非空的值。空串与「没有」等价：写进一个空命令等于没有。
    bool isEnabled() const;

    // 打开 / 关闭。`exePath` 在打开时写进命令行（一律加引号 —— 路径含空格时
    // 不加引号会被拆成两个参数）。返回 false 时 `why` 给一句能直接给用户看的说明。
    bool setEnabled(bool on, const QString &exePath, QString *why = nullptr);

    // 已启用时按当前 exe 路径重写一次；未启用时**什么都不做**（绝不顺手打开）。
    void refresh(const QString &exePath);

    // Run 键里那条命令的原文（诊断与测试用）。没有则空串。
    QString storedCommand() const;

private:
    QString runKeyPath_;
};

} // namespace lanrdp
