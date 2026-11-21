#include "app/GlobalHotkey.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QKeyCombination>
#include <QKeySequence>
#include <QWidget>

#include <utility>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace lanrdp {

namespace {

#ifdef Q_OS_WIN
UINT toWinModifiers(Qt::KeyboardModifiers modifiers)
{
    // MOD_NOREPEAT：按住不放不重复触发。这两个热键都是「切换」动作，
    // 连发一次就切回去了 —— 一个按住的手位不该让遮挡闪两下。
    UINT flags = MOD_NOREPEAT;
    if (modifiers & Qt::ControlModifier)
        flags |= MOD_CONTROL;
    if (modifiers & Qt::AltModifier)
        flags |= MOD_ALT;
    if (modifiers & Qt::ShiftModifier)
        flags |= MOD_SHIFT;
    if (modifiers & Qt::MetaModifier)
        flags |= MOD_WIN;
    return flags;
}

// Qt::Key_* → 虚拟键码。与 parseHotkeySequence 的白名单**严格对应**：
// 那边放行了什么这里就必须能翻出来，否则症状是「注册成功但永远收不到」。
UINT toVirtualKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return UINT(key);                                     // VK 'A'..'Z' = 0x41..0x5A
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return UINT(key);                                     // VK '0'..'9' = 0x30..0x39
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return UINT(0x70 + (key - Qt::Key_F1));               // VK_F1 = 0x70
    return 0;
}
#endif

} // namespace

bool parseHotkeySequence(const QString &sequence, HotkeyCombination *out)
{
    if (!out)
        return false;
    *out = HotkeyCombination{};

    const QString trimmed = sequence.trimmed();
    if (trimmed.isEmpty())
        return false;

    const QKeySequence parsed = QKeySequence::fromString(trimmed, QKeySequence::PortableText);
    // 多段序列（"Ctrl+K, Ctrl+C"）不是「按一下」，这里只接受单段。
    if (parsed.count() != 1)
        return false;

    const QKeyCombination combination = parsed[0];
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    const int key = combination.key();

    // 白名单之外的修饰位（KeypadModifier / GroupSwitchModifier）一律拒绝：
    // 它们没法映射成 MOD_*，静默丢掉就成了「绑了另一条组合」。
    const Qt::KeyboardModifiers allowed = Qt::ControlModifier | Qt::AltModifier
                                        | Qt::ShiftModifier | Qt::MetaModifier;
    if (modifiers & ~allowed)
        return false;
    // 必须带 Ctrl 或 Alt：不带修饰键的全局热键会把一个普通按键从整台机器上抢走
    //（RegisterHotKey 不拦你，但用户的 H 就再也打不出来了）。
    if (!(modifiers & (Qt::ControlModifier | Qt::AltModifier)))
        return false;

    const bool letter = key >= Qt::Key_A && key <= Qt::Key_Z;
    const bool digit  = key >= Qt::Key_0 && key <= Qt::Key_9;
    const bool func   = key >= Qt::Key_F1 && key <= Qt::Key_F24;
    if (!letter && !digit && !func)
        return false;

    out->modifiers = modifiers;
    out->key       = key;
    return true;
}

// ---------------------------------------------------------------------------
// Win32HotkeyPlatform
// ---------------------------------------------------------------------------

Win32HotkeyPlatform::Win32HotkeyPlatform()
{
    // 没有 QApplication 就没有 HWND，也就没有全局热键 —— 直接留空，
    // 让 register 一律失败（调用方本来就按「静默降级」处理）。
    if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
        return;

    // WA_DontShowOnScreen：只建 HWND，不真的挂到桌面上（RegisterHotKey 不要求可见）。
    // Qt::Tool：不进任务栏、不进 Alt+Tab —— 这个窗口不该留下任何可见痕迹。
    window_ = new QWidget(nullptr, Qt::Tool);
    window_->setAttribute(Qt::WA_DontShowOnScreen, true);
    window_->setObjectName(QStringLiteral("lanrdp-hotkey-host"));
    window_->createWinId();
}

Win32HotkeyPlatform::~Win32HotkeyPlatform()
{
    delete window_;
    window_ = nullptr;
}

bool Win32HotkeyPlatform::registerHotkey(int id, const HotkeyCombination &combination)
{
#ifdef Q_OS_WIN
    if (!window_ || !combination.isValid())
        return false;
    const UINT vk = toVirtualKey(combination.key);
    if (!vk)
        return false;
    return RegisterHotKey(reinterpret_cast<HWND>(window_->winId()), id,
                          toWinModifiers(combination.modifiers), vk) != 0;
#else
    Q_UNUSED(id);
    Q_UNUSED(combination);
    return false;
#endif
}

void Win32HotkeyPlatform::unregisterHotkey(int id)
{
#ifdef Q_OS_WIN
    if (!window_)
        return;
    UnregisterHotKey(reinterpret_cast<HWND>(window_->winId()), id);
#else
    Q_UNUSED(id);
#endif
}

// ---------------------------------------------------------------------------
// GlobalHotkey
// ---------------------------------------------------------------------------

GlobalHotkey::GlobalHotkey(QObject *parent)
    : QObject(parent)
{
    ownedPlatform_ = std::make_unique<Win32HotkeyPlatform>();
    platform_      = ownedPlatform_.get();
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installNativeEventFilter(this);
}

GlobalHotkey::~GlobalHotkey()
{
    unbindAll();   // 先把系统里那几条撤掉，别把热键留给下一个进程
    if (QCoreApplication *app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
}

void GlobalHotkey::setPlatform(HotkeyPlatform *platform)
{
    if (!platform || platform == platform_)
        return;
    unbindAll();   // 旧平台上注册的那几条必须先撤掉
    platform_ = platform;
}

bool GlobalHotkey::bind(int id, const QString &sequence)
{
    if (id <= 0)
        return false;

    HotkeyCombination combination;
    if (!parseHotkeySequence(sequence, &combination)) {
        emit bindFailed(id, sequence,
                        QStringLiteral("不是可用的组合（要 Ctrl 或 Alt + 字母 / 数字 / F1–F24）"));
        return false;
    }

    // 同一组合绑两次：RegisterHotKey 会失败，这里提前给出人话原因。
    for (const Binding &bound : std::as_const(bindings_)) {
        if (bound.id != id && bound.combination.modifiers == combination.modifiers
            && bound.combination.key == combination.key) {
            emit bindFailed(id, sequence, QStringLiteral("与已绑定的 %1 重复").arg(bound.sequence));
            return false;
        }
    }

    unbind(id);   // 同一个 id 换组合：先撤旧的，免得留下两条都指向同一个动作

    if (platform_->registerHotkey(id, combination)) {
        Binding binding;
        binding.id          = id;
        binding.combination = combination;
        binding.sequence    = sequence;
        bindings_.append(binding);
        return true;
    }

    emit bindFailed(id, sequence, QStringLiteral("注册失败：该组合可能已被别的程序占用"));
    return false;
}

void GlobalHotkey::unbind(int id)
{
    for (int i = bindings_.size() - 1; i >= 0; --i) {
        if (bindings_.at(i).id != id)
            continue;
        platform_->unregisterHotkey(id);
        bindings_.removeAt(i);
    }
}

void GlobalHotkey::unbindAll()
{
    for (const Binding &binding : std::as_const(bindings_))
        platform_->unregisterHotkey(binding.id);
    bindings_.clear();
}

void GlobalHotkey::activate(int id)
{
    // 只认自己绑过的 id：`WM_HOTKEY` 是**线程可见**的，别的组件（插件、
    // 将来自己再加的模块）也可能往这个队列里投 id。
    if (!bindingForId(id))
        return;
    emit activated(id);
}

const GlobalHotkey::Binding *GlobalHotkey::bindingForId(int id) const
{
    for (const Binding &binding : bindings_) {
        if (binding.id == id)
            return &binding;
    }
    return nullptr;
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result);
#ifdef Q_OS_WIN
    if (eventType != QByteArrayLiteral("windows_generic_MSG")
        && eventType != QByteArrayLiteral("windows_dispatcher_MSG"))
        return false;

    const MSG *msg = static_cast<const MSG *>(message);
    if (!msg || msg->message != WM_HOTKEY)
        return false;

    const int id = int(msg->wParam);
    if (!bindingForId(id))
        return false;

    activate(id);
    return true;   // 已经处理，别再往下走（否则会变成一条无人认领的按键消息）
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    return false;
#endif
}

} // namespace lanrdp
