#pragma once

#include "protocol/Frame.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

QT_BEGIN_NAMESPACE
class QClipboard;
QT_END_NAMESPACE

namespace lanrdp {

class Session;

// 「读写本地剪贴板」的可注入抽象。
//
// 为什么要加这一层而不是直接用 QClipboard：offscreen 平台的剪贴板是 no-op，
// 也不会发 dataChanged，没法驱动「回环抑制」的测试；单测注入一个假实现即可
// 完全离线地验证全部逻辑（见 tests/test_clipboardsync.cpp）。
class ClipboardPort : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardPort(QObject *parent = nullptr) : QObject(parent) {}
    ~ClipboardPort() override = default;

    virtual QString text() const = 0;
    virtual void setText(const QString &text) = 0;

signals:
    // 本地剪贴板内容变化（QClipboard::dataChanged 的抽象）
    void changed();
};

// 把系统 QClipboard 适配成 ClipboardPort（生产环境用）。
class QtClipboardPort : public ClipboardPort
{
    Q_OBJECT

public:
    explicit QtClipboardPort(QClipboard *clipboard, QObject *parent = nullptr);

    QString text() const override;
    void setText(const QString &text) override;

private:
    QClipboard *clipboard_ = nullptr;
};

// 剪贴板双向同步（仅纯文本）。
//
// 回环抑制叠加**两道**保险：
// ① 写入远端内容前置 applyingRemote_ 标志，标志期内忽略剪贴板的 changed；
// ② 文本与「上次收发的内容」比对，相同则不发帧、也不重复写入。
// 只靠 ① 不够 —— 某些平台的 dataChanged 是异步投递的，标志那时早被清掉了。
//
// 长度上限 kClipboardMaxBytes（见 protocol/Clipboard.h），超限丢弃并发 rejected。
// 只有设置了同步目标（setSession / setFrameSink）才会真正把帧发出去。
class ClipboardSync : public QObject
{
    Q_OBJECT

public:
    // 发送一帧的通道。生产环境绑到当前 Session；单测注入假实现。
    using FrameSink = std::function<void(const Frame &)>;

    explicit ClipboardSync(QObject *parent = nullptr);
    ~ClipboardSync() override;

    // 绑定系统剪贴板（生产用）。nullptr = 停止监听本地剪贴板。
    void setClipboard(QClipboard *clipboard);

    // 注入剪贴板端口（单测用）。端口所有权仍归调用方。
    void setPort(ClipboardPort *port);

    // 同步目标会话。nullptr = 暂停同步（未连接 / 无人持令牌）。
    void setSession(Session *session);
    Session *session() const { return session_; }

    // 直接注入发送通道（单测用，替代 setSession）。
    void setFrameSink(FrameSink sink);

    // 处理收到的帧；返回 true 表示本类消费了它（即 type == ClipboardText）。
    bool handleFrame(const Frame &frame);

signals:
    void sent(const QString &text);
    void received(const QString &text);
    void rejected(const QString &reason);

private slots:
    void onLocalChanged();

private:
    void installPort(ClipboardPort *port);
    void pushLocal();
    void applyRemote(const QString &text);

    // 不拥有：setClipboard 建的端口以 this 为父（由 QObject 回收），
    // setPort 注入的端口所有权归调用方。
    ClipboardPort *port_ = nullptr;
    Session       *session_ = nullptr;
    FrameSink      sink_;
    bool           applyingRemote_ = false;
    QString        lastSynced_;  // 上次「收发」过的文本（双向去重）
};

} // namespace lanrdp
