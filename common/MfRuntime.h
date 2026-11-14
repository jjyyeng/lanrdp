#pragma once

#include <QString>

#ifndef _HRESULT_DEFINED
using HRESULT = long;
#endif

namespace lanrdp {

// 确保进程已启动 Media Foundation、当前线程已初始化 COM。
// 可重复调用（MFStartup 进程内只执行一次）。失败返回 false。
//
// 本函数现在记录**每个线程**的初始化状态（COM 初始化是线程局部的，
// `MFStartup` 才是进程级的）。多次调用只记一次引用计数，配合 releaseMfRuntime()
// 释放 —— 工作线程结束前必须配对调用，否则该线程的 COM 永不撤销。
bool ensureMfRuntime();

// 撤销当前线程的 COM 初始化（与 ensureMfRuntime 配对）。
//
// 为什么要专门有这个：同步 MFT 有**线程亲和性**，所以解码器必须在工作线程内
// open()、并在同线程内 Release。线程退出前不 CoUninitialize 会漏掉该线程的
// COM 引用（进程级 MFStartup 的计数不需要我们管，MF 自己管进程）。
//
// 幂等：当前线程没初始化过（或已释放）时直接返回，不重复撤销。
// 刻意**不做**「撤销进程级 MFShutdown」—— MF 运行时随进程结束自然回收，
// 主动 shutdown 在多线程场景下反而容易踩顺序坑（YAGNI）。
void releaseMfRuntime();

// 把 HRESULT 变成可读的错误信息，便于写进 lastError()。
QString hrText(const char *what, HRESULT hr);

} // namespace lanrdp
