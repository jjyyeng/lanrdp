#include "host/MfRuntime.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>

#include <atomic>
#include <mutex>

namespace lanrdp {

namespace {

// COM 初始化是**线程局部**的，所以每个线程各记一份状态。
// thread_local 天然免锁；`succeeded` 记住本线程是否真的 CoInitializeEx 成功过
// （RPC_E_CHANGED_MODE = 别人已用别的套间模型初始化过，那时**不归我们撤销**）。
thread_local bool t_comInitialized = false;

} // namespace

bool ensureMfRuntime()
{
    // MFStartup 是进程级的，只需一次。
    static std::once_flag    once;
    static std::atomic<bool> mfOk{false};

    std::call_once(once, []() {
        mfOk = SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_FULL));
    });
    if (!mfOk)
        return false;

    if (t_comInitialized)
        return true;  // 本线程已初始化过，不要重复 CoInitializeEx（引用计数会涨）

    // Qt 可能已经初始化过 COM；模式不一致（RPC_E_CHANGED_MODE）同样可用 ——
    // 这种情况下套间**不是我们建的**，故不打标记、releaseMfRuntime() 也就不撤销。
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        t_comInitialized = true;
        return true;
    }
    return hr == RPC_E_CHANGED_MODE;
}

void releaseMfRuntime()
{
    if (!t_comInitialized)
        return;
    t_comInitialized = false;
    ::CoUninitialize();
}

QString hrText(const char *what, HRESULT hr)
{
    return QStringLiteral("%1 (hr=0x%2)")
        .arg(QLatin1String(what))
        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

} // namespace lanrdp
