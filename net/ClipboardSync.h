#pragma once

#include "protocol/Frame.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <cstdint>
#include <functional>

QT_BEGIN_NAMESPACE
class QClipboard;
class QTimer;
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

    // 剪贴板里的**本地文件**（资源管理器里 Ctrl+C 的那一批，绝对路径）。
    // 默认空 —— 只关心文本的假实现不必跟着改；返回空即退化为文本同步。
    // 目录不在其中：这一路只能传文件（见 ClipboardSync 的说明）。
    virtual QStringList files() const { return {}; }
    virtual void setFiles(const QStringList &paths) { Q_UNUSED(paths) }

signals:
    // 本地剪贴板内容变化（QClipboard::dataChanged 的抽象）
    void changed();
};

// 从剪贴板 mime 的 URL 列表里挑出**真正要传的文件**（绝对路径）：
// 非本地文件、目录、以及已经不存在的路径一律跳过。
//
// 混选「文件 + 目录」时把文件挑出来发，而不是整批不动 —— 用户复选一个目录加两个文件，
// 期望的显然是那两个文件；而目录本身传不了（FILE_* 是逐文件传的，没有「建目录 + 铺子树」）。
//
// 抽成自由函数是为了能离线测：QtClipboardPort::files() 要真剪贴板才驱动得起来，
// 而这段挑拣规则恰恰是「混选时到底发哪几个」的全部逻辑。
QStringList localFilesFromUrls(const QList<QUrl> &urls);

// 把系统 QClipboard 适配成 ClipboardPort（生产环境用）。
class QtClipboardPort : public ClipboardPort
{
    Q_OBJECT

public:
    explicit QtClipboardPort(QClipboard *clipboard, QObject *parent = nullptr);

    QString text() const override;
    void setText(const QString &text) override;
    QStringList files() const override;
    void setFiles(const QStringList &paths) override;

private:
    QClipboard *clipboard_ = nullptr;
};

// 剪贴板双向同步：纯文本 + **文件清单**。
//
// 回环抑制叠加**两道**保险：
// ① 写入远端内容前置 applyingRemote_ 标志，标志期内忽略剪贴板的 changed；
// ② 内容与「上次收发的内容」比对，相同则不发帧、也不重复写入。
// 只靠 ① 不够 —— 某些平台的 dataChanged 是异步投递的，标志那时早被清掉了。
//
// 长度上限 kClipboardMaxBytes（见 protocol/Clipboard.h），超限丢弃并发 rejected。
// 只有设置了同步目标（setSession / setFrameSink）才会真正把帧发出去。
//
// ---- 文件（v6）----
// 本地复制的文件**不会自动发**，只记成「待发」并通知界面（pendingFilesChanged）。
// 理由是复制文件极常见的目的是「在自己机器上换个地方放」—— 自动发过去既白占带宽，
// 又会把对方的剪贴板顶掉。要发给谁，由用户点**那一格**悬浮条上的【发送文件】决定；
// 目标因此是「点了哪一格」而不是「选中了哪一格」。
//
// 接收侧仍是自动的：收到清单后进入「凑批」状态，每落盘一个文件调一次
// noteReceivedFile()，凑齐了才把这批路径写进本地剪贴板 —— 于是本机 Ctrl+V 就能粘贴。
// 凑不齐（有一件失败 / 超时）就整批放弃，绝不往剪贴板里放半批文件。
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

    // ---- 「待发文件」----
    // 本机剪贴板里那批文件（绝对路径）。空 = 没有待发的。
    QStringList pendingLocalFiles() const { return pendingFiles_; }

    // 这批已经发出去（或用户不打算发）：清掉，界面上的按钮回到常态。
    // ⚠️ 不清「回环抑制」的记录 —— 剪贴板里如果还装着同一批文件，用户再复制一次
    // 仍该重新点亮，否则发过一次之后就再也发不出去了。
    void discardPendingLocalFiles();

    // 处理收到的帧；返回 true 表示本类消费了它（CLIPBOARD_TEXT / CLIPBOARD_FILES）。
    bool handleFrame(const Frame &frame);

    // 对方复制的那批文件里，有一个落盘了。凑齐即写本地剪贴板。
    // 不在「凑批」状态时什么也不做（普通发送文件的落盘不该动剪贴板）。
    void noteReceivedFile(const QString &path);

    // 放弃当前这批（有一个失败 / 连接断了）。不写剪贴板。
    void cancelIncomingFiles();

    // 还在等几个文件（0 = 没有在凑批）。给 UI 显示「正在接收 2/3」。
    int pendingFileCount() const
    {
        return int(incomingNames_.size() - incomingPaths_.size());
    }

    // 凑批超时（毫秒）。超时即整批放弃 —— 少一个文件时干等下去，
    // 界面会永远停在「接收中」，而剪贴板里什么也没有。
    void setFileAssembleTimeoutMs(int ms);

signals:
    void sent(const QString &text);
    void received(const QString &text);
    void rejected(const QString &reason);

    // ---- 文件 ----
    // 本机复制了一批文件，等着用户决定发不发；参数是那批文件的绝对路径。
    void pendingFilesChanged(const QStringList &paths);
    void filesIncoming(const QStringList &names);    // 收到清单，开始等这批文件
    void filesReceived(const QStringList &paths);    // 这批已写进本地剪贴板

private slots:
    void onLocalChanged();

private:
    void installPort(ClipboardPort *port);
    void pushLocal();
    void setPendingLocalFiles(const QStringList &paths);
    void applyRemote(const QString &text);
    void beginIncomingFiles(const QStringList &names);
    void finishIncomingFiles();

    // 不拥有：setClipboard 建的端口以 this 为父（由 QObject 回收），
    // setPort 注入的端口所有权归调用方。
    ClipboardPort *port_ = nullptr;
    Session       *session_ = nullptr;
    FrameSink      sink_;
    QTimer        *assembleTimer_ = nullptr;
    bool           applyingRemote_ = false;
    QString        lastSynced_;      // 上次「收发」过的文本（双向去重）
    QStringList    lastFiles_;       // 上次「收发」过的文件清单（双向去重）
    QStringList    pendingFiles_;    // 本机已复制、还没发出去的那批
    QStringList    incomingNames_;   // 对方说这批有几个文件（文件名）
    QStringList    incomingPaths_;   // 已落盘的那几个
};

} // namespace lanrdp
