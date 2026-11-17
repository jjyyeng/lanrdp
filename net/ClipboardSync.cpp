#include "net/ClipboardSync.h"

#include "net/Session.h"
#include "protocol/Clipboard.h"

#include <QClipboard>
#include <QFileInfo>
#include <QMimeData>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace lanrdp {

namespace {

// 凑批超时：清单说有 3 个，只来了 2 个，就一直等下去？
// 30 秒足够慢速链路把一批小文件送完；再等就是在骗自己（那一个多半失败了，
// 或者对方中途断开），而界面会永远停在「接收中」、剪贴板里空空如也。
constexpr int kFileAssembleTimeoutMs = 30000;

} // namespace

// ---------------------------------------------------------------------------
// QtClipboardPort
// ---------------------------------------------------------------------------

QtClipboardPort::QtClipboardPort(QClipboard *clipboard, QObject *parent)
    : ClipboardPort(parent)
    , clipboard_(clipboard)
{
    if (clipboard_)
        connect(clipboard_, &QClipboard::dataChanged, this, &ClipboardPort::changed);
}

QString QtClipboardPort::text() const
{
    return clipboard_ ? clipboard_->text(QClipboard::Clipboard) : QString();
}

void QtClipboardPort::setText(const QString &text)
{
    if (clipboard_)
        clipboard_->setText(text, QClipboard::Clipboard);
}

QStringList localFilesFromUrls(const QList<QUrl> &urls)
{
    QStringList out;
    out.reserve(urls.size());
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        // 目录不传：FILE_* 传的是一份份文件，没有「建目录 + 铺子树」这回事。
        // 只挑文件还有一个副作用是想要的 —— 混选时把文件挑出来发，别整批不动。
        // isFile() 对不存在的路径也是 false，顺手把「复制完就被删/搬走」的挡掉。
        if (QFileInfo(path).isFile())
            out.append(path);
    }
    return out;
}

QStringList QtClipboardPort::files() const
{
    if (!clipboard_)
        return {};

    const QMimeData *mime = clipboard_->mimeData(QClipboard::Clipboard);
    if (!mime || !mime->hasUrls())
        return {};

    return localFilesFromUrls(mime->urls());
}

void QtClipboardPort::setFiles(const QStringList &paths)
{
    if (!clipboard_ || paths.isEmpty())
        return;

    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString &path : paths)
        urls.append(QUrl::fromLocalFile(path));

    // setMimeData 接管所有权。⚠️ 必须写 Clipboard 模式：Selection 模式在 Windows
    // 上不存在，而「能粘贴」靠的正是标准剪贴板。
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    clipboard_->setMimeData(mime, QClipboard::Clipboard);
}

// ---------------------------------------------------------------------------
// ClipboardSync
// ---------------------------------------------------------------------------

ClipboardSync::ClipboardSync(QObject *parent)
    : QObject(parent)
{
    assembleTimer_ = new QTimer(this);
    assembleTimer_->setSingleShot(true);
    assembleTimer_->setInterval(kFileAssembleTimeoutMs);
    connect(assembleTimer_, &QTimer::timeout, this, [this] {
        const int missing = pendingFileCount();
        cancelIncomingFiles();
        emit rejected(tr("有 %1 个文件没有送达，这批未写进剪贴板").arg(missing));
    });
}

ClipboardSync::~ClipboardSync() = default;

void ClipboardSync::installPort(ClipboardPort *port)
{
    if (port_ == port)
        return;

    if (port_) {
        disconnect(port_, nullptr, this, nullptr);
        // 只有 setClipboard 建的端口是本对象的子对象，可以安全回收；
        // setPort 注入的端口所有权归调用方。
        if (port_->parent() == this)
            port_->deleteLater();
    }

    port_ = port;
    if (port_)
        connect(port_, &ClipboardPort::changed, this, &ClipboardSync::onLocalChanged);
}

void ClipboardSync::setClipboard(QClipboard *clipboard)
{
    if (!clipboard) {
        installPort(nullptr);
        return;
    }
    installPort(new QtClipboardPort(clipboard, this));
}

void ClipboardSync::setPort(ClipboardPort *port)
{
    installPort(port);
}

void ClipboardSync::setSession(Session *session)
{
    session_ = session;
    if (session_)
        sink_ = [this](const Frame &frame) { session_->sendFrame(frame); };
    else
        sink_ = nullptr;
}

void ClipboardSync::setFrameSink(FrameSink sink)
{
    sink_ = std::move(sink);
}

void ClipboardSync::discardPendingLocalFiles()
{
    if (pendingFiles_.isEmpty())
        return;

    // ⚠️ 这里**不动** lastFiles_：它只负责「认出来这是远端刚写进剪贴板的那批」。
    // 顺手把它记成「已处理」会让「同一批文件再复制一次」被误吞 —— 而剪贴板里确实
    // 还装着那批文件，按钮亮着才对。
    pendingFiles_.clear();
    emit pendingFilesChanged(pendingFiles_);
}

void ClipboardSync::setFileAssembleTimeoutMs(int ms)
{
    assembleTimer_->setInterval(ms);
}

bool ClipboardSync::handleFrame(const Frame &frame)
{
    if (frame.type == static_cast<std::uint8_t>(FrameType::ClipboardFiles)) {
        const auto decoded = decodeClipboardFiles(frame.payload);
        if (!decoded)
            return true;   // 属于本类的帧，但载荷非法 -> 丢弃（不断连）
        beginIncomingFiles(decoded->names);
        return true;
    }

    if (frame.type != static_cast<std::uint8_t>(FrameType::ClipboardText))
        return false;

    const auto decoded = decodeClipboardText(frame.payload);
    if (!decoded)
        return true;  // 属于本类的帧，但载荷非法 -> 丢弃（不断连）

    applyRemote(decoded->text);
    return true;
}

void ClipboardSync::onLocalChanged()
{
    if (applyingRemote_)  // 回环抑制 ①：这是我们自己刚写进去的
        return;
    pushLocal();
}

void ClipboardSync::pushLocal()
{
    if (!port_)
        return;

    // 文件优先于文本：Explorer 复制文件时往往**同时**放一份「文件名当文本」，
    // 先看文本的话就会发过去一串路径字符串，而不是那些文件。
    // 但文件本身**不自动发** —— 只记成待发，等用户在某一格的条上点【发送文件】。
    const QStringList paths = port_->files();
    if (!paths.isEmpty()) {
        setPendingLocalFiles(paths);
        return;
    }
    if (!pendingFiles_.isEmpty())
        discardPendingLocalFiles();   // 剪贴板已经换成别的东西了

    const QString text = port_->text();
    if (text == lastSynced_)  // 回环抑制 ② / 内容去重
        return;
    if (!sink_)  // 没有同步目标（未连接 / 无人持令牌）：静默暂停
        return;

    const auto payload = encodeClipboardText(text);
    if (!payload) {
        emit rejected(tr("剪贴板文本超过 %1 字节上限，未同步")
                          .arg(static_cast<qulonglong>(kClipboardMaxBytes)));
        return;
    }

    Frame frame;
    frame.type    = static_cast<std::uint8_t>(FrameType::ClipboardText);
    frame.payload = *payload;
    sink_(frame);

    lastSynced_  = text;
    emit sent(text);
}

void ClipboardSync::setPendingLocalFiles(const QStringList &paths)
{
    if (paths == lastFiles_)  // 回环抑制 ②：这是刚由远端写进剪贴板的那一批
        return;
    if (paths == pendingFiles_)   // 同一批又被触发了一次（内容没变）
        return;

    pendingFiles_ = paths;
    emit pendingFilesChanged(pendingFiles_);
}

void ClipboardSync::applyRemote(const QString &text)
{
    if (text == lastSynced_)  // 相同内容不重复写入、不重复上报
        return;

    applyingRemote_ = true;
    if (port_)
        port_->setText(text);
    applyingRemote_ = false;

    lastSynced_ = text;
    emit received(text);
}

void ClipboardSync::beginIncomingFiles(const QStringList &names)
{
    // 上一次没凑齐又来了新的：以新的为准（晚到的多半是对方又复制了一次）。
    incomingNames_ = names;
    incomingPaths_.clear();
    assembleTimer_->start();
    emit filesIncoming(names);
}

void ClipboardSync::noteReceivedFile(const QString &path)
{
    if (incomingNames_.isEmpty())
        return;   // 不在凑批状态：这是「发送文件…」或拖入的那类，不动剪贴板

    if (path.isEmpty()) {
        cancelIncomingFiles();
        return;
    }

    incomingPaths_.append(path);
    if (incomingPaths_.size() < incomingNames_.size()) {
        assembleTimer_->start();   // 还在来，重新计时
        return;
    }

    finishIncomingFiles();
}

void ClipboardSync::cancelIncomingFiles()
{
    assembleTimer_->stop();
    incomingNames_.clear();
    incomingPaths_.clear();
}

void ClipboardSync::finishIncomingFiles()
{
    const QStringList paths = incomingPaths_;
    cancelIncomingFiles();

    applyingRemote_ = true;
    if (port_)
        port_->setFiles(paths);
    applyingRemote_ = false;

    // 记下来：刚写进去的这批如果晚一拍触发 changed，第二道保险会拦住它
    //（被控端本来没有文件通道，但将来若加上，这里就是防止两端互相回传的那道闸）。
    lastFiles_ = paths;
    emit filesReceived(paths);
}

} // namespace lanrdp
