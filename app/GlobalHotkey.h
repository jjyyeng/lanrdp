#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class QWidget;

namespace lanrdp {

// 一条全局热键的组合（与平台无关的形式）。
// 刻意不用 QKeySequence 存：那是「给人编辑的文本」，这里是「要下发给
// RegisterHotKey 的两个数字」，混在一起会让「解析」与「下发」两件事纠缠不清。
struct HotkeyCombination
{
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    int                   key       = 0;   // Qt::Key_*

    bool isValid() const { return key != 0; }
};

// `"Ctrl+Alt+F11"` → { Ctrl|Alt, Qt::Key_F11 }。纯函数，独立单测。
//
// 只认 Ctrl / Alt / Shift / Win + `A`–`Z` / `0`–`9` / `F1`–`F24`，其余一律拒绝：
// 需要的再加，猜出来的映射错了比「不支持」更难查。
// 另外**必须带 Ctrl 或 Alt** —— 不带修饰键的全局热键会把一个普通按键从整台机器上
// 抢走（`RegisterHotKey` 不拦你，但用户的 `H` 就再也打不出来了）。
bool parseHotkeySequence(const QString &sequence, HotkeyCombination *out);

// 平台那一层。抽成接口只有一个理由：没有桌面会话的环境里 `RegisterHotKey`
// 必然失败，而「组合被别的程序占用时静默降级」这条行为必须能被断言 ——
// 拿真机去碰运气不算验证。
class HotkeyPlatform
{
public:
    virtual ~HotkeyPlatform() = default;

    virtual bool registerHotkey(int id, const HotkeyCombination &combination) = 0;
    virtual void unregisterHotkey(int id)                                     = 0;
};

// 真正的 Windows 实现：热键挂在一个**永不显示**的窗口上，
// `WM_HOTKEY` 由 `GlobalHotkey` 以 `QAbstractNativeEventFilter` 的身份收下
//（Qt 的窗口过程会把原始消息转给已安装的过滤器）。
//
// 为什么不用「隐藏消息窗 + 自绘 WndProc」或「独立线程跑消息循环」：那要多写
// 注册窗口类 / 子类化 thunk / 线程收尾三摊易错代码，而这里只需要一个 HWND。
class Win32HotkeyPlatform : public HotkeyPlatform
{
public:
    Win32HotkeyPlatform();
    ~Win32HotkeyPlatform() override;

    bool registerHotkey(int id, const HotkeyCombination &combination) override;
    void unregisterHotkey(int id) override;

private:
    QWidget *window_ = nullptr;   // 只为了给 RegisterHotKey 一个 HWND
};

// 全局热键的唯一出口：注册 / 注销 / 收消息。
//
// 失败一律**静默降级**：发 `bindFailed`（调用方记一行日志），不弹窗、不抛。
// 理由是这个功能本身就有一条不用热键的路（菜单项），而热键被占用是常态
//（输入法 / 显卡驱动 / 截图工具都在抢 Ctrl+Alt+X）。
class GlobalHotkey : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;

    // 注入平台实现（测试用）。**不接管所有权**，且必须比本对象活得久 ——
    // 析构时本类会回头调 `platform_->unregisterHotkey()` 把热键撤干净。
    void setPlatform(HotkeyPlatform *platform);

    // 绑定一条组合。返回 false 时原因已由 `bindFailed` 给出。
    // 同一个 id 重复绑定 = 先解绑再绑；同一组合绑两次 = 直接失败
    //（`RegisterHotKey` 对同一组合会返回失败，这里提前给出更可读的原因）。
    bool bind(int id, const QString &sequence);
    void unbind(int id);
    void unbindAll();

    // 收到一条热键。`nativeEventFilter` 匹配到 `WM_HOTKEY` 后调它；
    // 测试可以直接调，不必真的占一个系统热键。
    void activate(int id);

signals:
    void activated(int id);
    void bindFailed(int id, const QString &sequence, const QString &reason);

protected:
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    struct Binding
    {
        int               id = 0;
        HotkeyCombination combination;
        QString           sequence;
    };

    const Binding *bindingForId(int id) const;

    std::unique_ptr<HotkeyPlatform> ownedPlatform_;
    HotkeyPlatform                 *platform_ = nullptr;
    QVector<Binding>                bindings_;
};

} // namespace lanrdp
