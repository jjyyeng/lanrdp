#include "net/ClipboardSync.h"

#include "net/Session.h"
#include "protocol/Clipboard.h"

#include <QClipboard>

#include <utility>

namespace lanrdp {

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

// ---------------------------------------------------------------------------
// ClipboardSync
// ---------------------------------------------------------------------------

ClipboardSync::ClipboardSync(QObject *parent)
    : QObject(parent)
{
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

bool ClipboardSync::handleFrame(const Frame &frame)
{
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

    const QString text = port_->text();
    if (text == lastSynced_)  // 回环抑制 ② / 内容去重
        return;
    if (!sink_)  // 没有同步目标（未连接 / 无人持令牌）：静默暂停
        return;

    const auto payload = encodeClipboardText(text);
    if (!payload) {
        emit rejected(QStringLiteral("剪贴板文本超过 %1 字节上限，未同步")
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

} // namespace lanrdp
