#include "app/Dialogs.h"

#include "app/PasswordEdit.h"
#include "common/LanAddresses.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include "net/Discovery.h"

namespace lanrdp {

QString defaultReceiveDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!dir.isEmpty())
        return dir;
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("received"));
}

namespace {

// 端口上限 65535 —— 用 QSpinBox 而不是 QLineEdit，把「输入非法端口」这件事
// 从「运行时校验 + 报错」变成「根本输不进去」（前者是白给的一类 bug）。
constexpr int kMinPort = 1;
constexpr int kMaxPort = 65535;

// 两个启动对话框的最小宽度。
//
// 取值来自「最长那条说明文字要能单行放下」：【开始监听…】里的口令报错与防火墙
// 提示，以及接收目录那种长路径框。QFormLayout 算 sizeHint 时会把可换行的标签
// 算得很窄，不兜底的话对话框会缩到 250px，两段话都断在词中间。
// 两个对话框共用同一个值 —— 它们是一对，宽度不一致切换时窗口会跳。
constexpr int kDialogMinWidth = 320;

// 码率档位。这个值是**上限**（自适应只往下调、绝不上探），所以宁可给宽一点：
// 设小了画面会长期顶在上限、糊在细节上；设大了最坏情况也不过是网络差时自己降。
// 档位是整数倍、便于用户口算带宽占用（4 路 × 16 Mbps ≈ 64 Mbps，千兆无线压力也不大）。
struct BitrateChoice
{
    int         bps;
    const char *label;
};

constexpr BitrateChoice kBitrateChoices[] = {
    {4 * 1000 * 1000, "4 Mbps"},
    {8 * 1000 * 1000, "8 Mbps"},
    {12 * 1000 * 1000, "12 Mbps"},
    {16 * 1000 * 1000, "16 Mbps"},
};

// 「流量未加密」提示。TLS 定为不做（局域网自用），但取舍必须摊在用户眼前 ——
// 尤其要挡住「把监听端口映射到公网」这种误用，那是本项目最坏的一种用法。
//
// 两个入口各一条，而不是只在别处交代一句：用户是在弹窗里做决定的。
// 颜色由主题 QSS 的 #insecureHint 规则给出（warning 档，比正文醒目、又不是错误）。
QLabel *makeInsecureHint(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(QStringLiteral("insecureHint"));
    label->setWordWrap(true);
    label->setText(QCoreApplication::translate(
        "lanrdp",
        "流量未加密，请仅在可信局域网内使用，不要把端口映射到公网。"));
    return label;
}

} // namespace

// ---------------------------------------------------------------------------
// 【开始监听…】
// ---------------------------------------------------------------------------

HostConfigDialog::HostConfigDialog(const Config &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("开始监听"));
    setMinimumWidth(kDialogMinWidth);

    auto *form = new QFormLayout();
    form->setVerticalSpacing(8);
    form->setContentsMargins(0, 0, 0, 0);

    port_ = new QSpinBox(this);
    port_->setObjectName(QStringLiteral("portEdit"));
    port_->setRange(kMinPort, kMaxPort);
    port_->setValue(int(initial.port));
    port_->setToolTip(tr("通常无需修改。若提示端口被占用，请更换为 1024 以上的其他端口。"));

    password_ = new PasswordEdit(this);
    password_->setText(initial.password);
    password_->setObjectName(QStringLiteral("passwordEdit"));
    password_->setPlaceholderText(tr("至少 6 位"));
    password_->setToolTip(tr("对方连接本机时必须输入相同的访问口令。"));

    // 口令不合规时的红字，**平时不占位**（隐藏）—— 点【开始监听】才校验。
    // 预先禁用按钮的话，用户输错时看不出为什么，比报错更难受。
    passwordError_ = new QLabel(this);
    passwordError_->setObjectName(QStringLiteral("passwordErrorHint"));
    passwordError_->setWordWrap(true);
    passwordError_->setVisible(false);

    // 空串（没存过 / 用户清空过）按默认目录预填。空框在这里没有意义：真正收文件时
    // 也是退到同一个默认目录（MainWindow::receiveDir()），显示一个空的只是让人以为
    // "没设，得自己选"。
    const QString recvDir = initial.receiveDir.trimmed().isEmpty()
        ? defaultReceiveDir()
        : initial.receiveDir;
    recvDir_ = new QLineEdit(recvDir, this);
    recvDir_->setObjectName(QStringLiteral("recvDirEdit"));
    recvDir_->setToolTip(tr(
        "对方发送的文件将保存到此目录（不存在时自动创建）。\n"
        "文件重名时自动追加 -1、-2 等后缀，不会覆盖已有文件。"));
    auto *browse = new QPushButton(tr("浏览…"), this);
    browse->setObjectName(QStringLiteral("browseRecvDirButton"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString picked = QFileDialog::getExistingDirectory(
            this, tr("选择接收目录"), recvDir_->text());
        if (!picked.isEmpty())
            recvDir_->setText(picked);
    });
    auto *dirRow = new QHBoxLayout();
    dirRow->setContentsMargins(0, 0, 0, 0);
    dirRow->addWidget(recvDir_, 1);
    dirRow->addWidget(browse);

    h264_ = new QCheckBox(tr("压缩画面传输（推荐）"), this);
    h264_->setObjectName(QStringLiteral("h264Check"));
    h264_->setChecked(initial.useH264);
    h264_->setToolTip(tr(
        "开启后画面先压缩再传输，可显著降低带宽占用，网络不佳时更流畅。\n"
        "仅排查问题时需要关闭。"));

    bitrate_ = new QComboBox(this);
    bitrate_->setObjectName(QStringLiteral("bitrateCombo"));
    for (const BitrateChoice &choice : kBitrateChoices)
        bitrate_->addItem(QString::fromLatin1(choice.label), choice.bps);
    // 非正值按默认档处理：否则下拉里会多出一条「0.0 Mbps（当前）」，
    // 而 config() 又把 0 兜回默认值 —— 界面与实际对不上，是最难查的那种不一致。
    const int wantedBitrate = initial.bitrateBps > 0 ? initial.bitrateBps : Config{}.bitrateBps;
    // 命令行 `--bitrate` 收的是任意正整数，不在档位里就补一条当前值 ——
    // 四舍五入到最近档等于**静默改掉**用户明确设过的数字，那是另一种"不生效"。
    if (bitrate_->findData(wantedBitrate) < 0) {
        bitrate_->addItem(tr("%1 Mbps（当前）")
                              .arg(wantedBitrate / 1000000.0, 0, 'f', 1),
                          wantedBitrate);
    }
    bitrate_->setCurrentIndex(bitrate_->findData(wantedBitrate));
    bitrate_->setToolTip(tr(
        "画面压缩后允许通过的码率上限，实际码率会随网络状况自动下调。\n"
        "画面越大、细节越多（如 4K），这个值越要往高选。"));
    // 裸帧路径没有码率可调（帧直接下发，不经过编码器），勾掉压缩时这一项没有意义。
    bitrate_->setEnabled(initial.useH264);
    connect(h264_, &QCheckBox::toggled, bitrate_, &QWidget::setEnabled);

    form->addRow(tr("监听端口"), port_);

    // 口令行：输入框 + 【生成】。
    //
    // 生成 12 位随机串（排除易混字符，规则见 PasswordPolicy.h）。生成后**自动切到
    // 可见** —— 这个动作本身就意味着「我要把它念给旁边的人」，藏着反而容易抄错；
    // 不想明文再点一下眼睛即可，状态不落盘。
    auto *passwordRow = new QWidget(this);
    auto *passwordLayout = new QHBoxLayout(passwordRow);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    passwordLayout->setSpacing(6);
    passwordLayout->addWidget(password_);

    auto *generateButton = new QPushButton(tr("生成"), passwordRow);
    generateButton->setObjectName(QStringLiteral("generatePasswordButton"));
    generateButton->setToolTip(tr(
        "生成一个随机口令，不使用容易看错的字符（0 / O / 1 / l / I）。"));
    passwordLayout->addWidget(generateButton);

    form->addRow(tr("访问口令"), passwordRow);
    form->addRow(passwordError_);   // 横跨两列（挤在字段列里会断在词中间）；平时隐藏

    // 「连我」：把对方要填的地址直接摆出来，省掉「先去查本机 IP」这一步。
    //
    // 本机常有多条路（有线 + 无线 + 虚拟网卡），选错地址是局域网工具最典型的首连
    // 失败；这里只列真实可达的那几条（过滤逻辑在 common/LanAddresses）。
    // 端口不写死在这行里 —— 用户随时会改，复制时实时读。
    auto *addressRow    = new QWidget(this);
    auto *addressLayout = new QHBoxLayout(addressRow);
    addressLayout->setContentsMargins(0, 0, 0, 0);
    addressLayout->setSpacing(6);

    const QStringList localAddrs = lanAddresses();
    lanAddresses_ = localAddrs;
    auto *addressLabel = new QLabel(lanAddressSummary(localAddrs), addressRow);
    addressLabel->setObjectName(QStringLiteral("lanAddressLabel"));
    addressLabel->setWordWrap(true);
    addressLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    addressLayout->addWidget(addressLabel, 1);

    auto *copyAddressButton = new QPushButton(tr("复制"), addressRow);
    copyAddressButton->setObjectName(QStringLiteral("copyLanAddressButton"));
    copyAddressButton->setToolTip(tr("复制成「地址:端口」，直接发给对方。"));
    copyAddressButton->setEnabled(!localAddrs.isEmpty());
    addressLayout->addWidget(copyAddressButton);

    form->addRow(tr("对方可用地址"), addressRow);
    form->addRow(tr("接收目录"), dirRow);
    form->addRow(h264_);   // 横跨两列
    form->addRow(tr("画面码率"), bitrate_);

    // 显卡色彩转换：默认开，与码率同属「画面」这一组。
    //
    // 为什么要让用户能关：显卡路径的失败**全是静默的**（驱动不给 VideoProcessor、
    // 输出色彩空间被忽略、Blt 返回失败），出问题时用户手里得有一个「立刻把它排除掉」
    // 的开关，而不是等改代码重打包。关掉只是慢（4K 每帧多约 10 ms），画面完全一样。
    gpuConvert_ = new QCheckBox(tr("使用显卡做色彩转换（推荐）"), this);
    gpuConvert_->setObjectName(QStringLiteral("gpuConvertCheck"));
    gpuConvert_->setChecked(initial.gpuConvert);
    gpuConvert_->setToolTip(tr(
        "把抓到的画面交给显卡转成编码器要的格式，大画面（如 4K）下明显更快。\n"
        "关掉后改用 CPU，画面不变、只是变慢；仅在怀疑显卡路径有问题时关闭。"));
    // 裸帧路径不经过编码器，压根不需要转格式 —— 这一项对它没有意义，与码率同口径。
    gpuConvert_->setEnabled(initial.useH264);
    connect(h264_, &QCheckBox::toggled, gpuConvert_, &QWidget::setEnabled);

    form->addRow(gpuConvert_);   // 横跨两列

    // 未加密提示放在防火墙提示之前：它比「连不上」更要紧。
    form->addRow(makeInsecureHint(this));

    // 防火墙提示。
    //
    // Windows 只在某个端口**第一次**被监听时询问一次；用户当时点了跳过，
    // 系统就会生成一条阻止规则、之后不再询问 —— 那时从程序侧完全看不出原因
    // （本机 listen 照常成功，只是外部连不进来）。
    //
    // 这里刻意**不自动加规则**：那需要管理员权限，而这辈子只点一两次，
    // 不值得为此在程序里开一条提权链路。写明「连不上就去放行」就够。
    //
    // 文案**不提网络类型**：弹窗的复选框里「专用 / 公用」两项，勾哪个取决于
    // 用户网卡被判成哪一类（同一台机器不同网卡分属两类很常见），指错一个
    // 方向等于没放行。让用户自己去防火墙里放行，两种情况都覆盖。
    //
    // 颜色用 textSecondary（不是 formHint 那档 muted）：这段关系到能不能连上，
    // muted 压在深色底上只有 3.3:1，读起来太吃力。
    auto *firewallHint = new QLabel(this);
    firewallHint->setObjectName(QStringLiteral("firewallHint"));
    firewallHint->setWordWrap(true);
    // 单参数 addRow 的 label 会紧贴上一行（表单的 verticalSpacing 管不到它的上方），
    // 补一点上边距，否则复选框和这段说明会挤成一块。
    firewallHint->setContentsMargins(0, 6, 0, 0);
    firewallHint->setText(tr("若连接不上，请在 Windows 防火墙中放行本程序。"));
    form->addRow(firewallHint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("开始监听"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));

    // 【复制邀请串】放在按钮区左侧（ActionRole）：它不结束对话框，却是这个对话框
    // 最常用的一步 —— 把地址、端口、口令一次抄给对方，省掉「先查 IP、再抄口令」。
    //
    // 与上面地址行那个【复制】分工：那边只复制「地址:端口」（可以发给任何人），
    // 这边连口令一起复制（等于一次性把进门凭据交出去）。两件事刻意分成两个按钮。
    invite_ = buttons->addButton(tr("复制邀请串"), QDialogButtonBox::ActionRole);
    invite_->setObjectName(QStringLiteral("copyInviteButton"));
    invite_->setToolTip(tr("复制「地址 / 端口 / 口令」三行，直接发给对方照填。"));
    // 没有局域网地址时复制出来也没有意义（对面拿不到地址），与上面【复制】同口径。
    invite_->setEnabled(!lanAddresses_.isEmpty());
    // 校验在 accept() 覆写里（唯一入口：点按钮和直接调 accept() 走同一条路）。
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // 一旦开始改就把上一轮的报错收起来（否则改对了红字还杵在那儿）。
    connect(password_, &QLineEdit::textChanged, this, [this](const QString &) {
        passwordError_->setVisible(false);
    });

    connect(generateButton, &QPushButton::clicked, this, [this] {
        password_->setText(generatePassword());
        password_->setPasswordVisible(true);
        passwordError_->setVisible(false);   // setText 已触发上面的收起，这里显式再说一次意图
    });

    connect(copyAddressButton, &QPushButton::clicked, this, [this, localAddrs] {
        // 端口实时读：用户可能刚把上面的输入框改过。
        QGuiApplication::clipboard()->setText(
            connectionCardText(localAddrs, quint16(port_->value())));
    });
    connect(invite_, &QPushButton::clicked, this, [this] { copyInvite(); });

    auto *box = new QVBoxLayout(this);
    box->addLayout(form);
    box->addWidget(buttons);
}

HostConfigDialog::Config HostConfigDialog::config() const
{
    Config c;
    c.port       = quint16(port_->value());
    c.password   = password_->text();
    c.receiveDir = recvDir_->text().trimmed();
    c.useH264    = h264_->isChecked();
    c.gpuConvert = gpuConvert_->isChecked();
    // currentData() 只有在条目被外部清空时才会拿到无效值，兜底回默认档。
    const int bps = bitrate_->currentData().toInt();
    c.bitrateBps  = bps > 0 ? bps : Config{}.bitrateBps;
    return c;
}

QString HostConfigDialog::inviteString() const
{
    return lanrdp::inviteText(lanAddresses_, quint16(port_->value()), password_->text());
}

// 邀请串里没有口令就只是一段「你能找到我，但进不来」的文字。与其复制一段
// 看起来能用、实际连不上的东西，不如就地拦下 —— 复用口令那行红字，
// 保住「同一个问题只在一处说明」的既有分工。
void HostConfigDialog::copyInvite()
{
    if (!isValidPassword(password_->text())) {
        passwordError_->setText(tr("先填好口令，邀请串里要带上它。"));
        passwordError_->setVisible(true);
        password_->setFocus();
        return;
    }
    QGuiApplication::clipboard()->setText(inviteString());

    // 复制是**静默**动作：剪贴板变了，界面上什么都不会动。按钮自己报一声，
    // 否则用户会怀疑到底点没点上，然后连点好几下。
    // 用 QPointer 而不是裸指针：反馈定时器可能在对话框关掉之后才到期。
    invite_->setText(tr("已复制"));
    QPointer<QPushButton> guard(invite_);
    QTimer::singleShot(1200, this, [guard] {
        if (guard)
            guard->setText(tr("复制邀请串"));
    });
}

// 口令不合规时不关窗口，并说明原因（对标同文件 ConnectDialog::accept()）。
//
// 空口令尤其要拦：两端都是空串时 sha256("") 相同，**握手会通过** ——
// 所以空口令不是「连不上」，而是「任何口令都能连」，被控端会退化成
// 一个没有凭据的远程键鼠注入服务。
//
// 长度与字符集的规则在 app/PasswordPolicy.h，与 `--host` 共用同一个判定口。
void HostConfigDialog::accept()
{
    if (!isValidPassword(password_->text())) {
        passwordError_->setText(
            tr("口令至少 6 位，且只能用英文字母、数字和符号。"));
        passwordError_->setVisible(true);
        password_->setFocus();
        return;   // 不关窗口：让用户就地改
    }
    QDialog::accept();
}

// ---------------------------------------------------------------------------
// 【新连接…】
// ---------------------------------------------------------------------------

ConnectDialog::ConnectDialog(const Config &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("新连接"));
    setMinimumWidth(kDialogMinWidth);   // 与【开始监听…】同宽，来回切换时窗口不跳

    auto *form = new QFormLayout();
    form->setVerticalSpacing(8);
    form->setContentsMargins(0, 0, 0, 0);

    host_ = new QLineEdit(initial.host, this);
    host_->setObjectName(QStringLiteral("peerHostEdit"));
    host_->setToolTip(tr("远程电脑的 IP 地址或主机名。"));

    port_ = new QSpinBox(this);
    port_->setObjectName(QStringLiteral("peerPortEdit"));
    port_->setRange(kMinPort, kMaxPort);
    port_->setValue(int(initial.port));

    password_ = new PasswordEdit(this);
    password_->setText(initial.password);
    password_->setObjectName(QStringLiteral("connectPasswordEdit"));
    password_->setPlaceholderText(tr("至少 6 位"));

    // 与【开始监听…】同款的红字：平时隐藏，点【连接】才校验。
    // 地址与口令共用这一条 —— 两类错误都属于「这一栏没填对」，不该用两种呈现方式。
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("connectErrorHint"));
    error_->setWordWrap(true);
    error_->setVisible(false);

    form->addRow(tr("对方地址"), host_);
    form->addRow(tr("对方端口"), port_);
    form->addRow(tr("访问口令"), password_);
    form->addRow(error_);   // 横跨两列；平时隐藏
    form->addRow(makeInsecureHint(this));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("连接"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // 一旦开始改就把上一轮的报错收起来（否则改对了红字还杵在那儿）。
    // 两栏都接：只接口令栏的话，「地址为空」的红字会一直留到下次点【连接】。
    connect(password_, &QLineEdit::textChanged, this, [this](const QString &) {
        error_->setVisible(false);
    });
    connect(host_, &QLineEdit::textChanged, this, [this](const QString &) {
        error_->setVisible(false);
    });

    // ---- 自动发现：局域网里正在等人的机器 ----
    // 默认**隐藏**：一台都还没发现时露一个空框，比没有这块更让人困惑。
    peersLabel_ = new QLabel(tr("局域网内正在等人的电脑"), this);
    peersLabel_->setObjectName(QStringLiteral("peerListHeader"));
    peersLabel_->setVisible(false);

    peers_ = new QListWidget(this);
    peers_->setObjectName(QStringLiteral("peerList"));
    peers_->setVisible(false);
    peers_->setSelectionMode(QAbstractItemView::SingleSelection);
    // 点一下就填：这条列表存在的意义就是省掉「去对方机器上问 IP」这一步，
    // 还要双击才生效等于多一道没理由的门槛。
    connect(peers_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        host_->setText(item->data(Qt::UserRole).toString());
        port_->setValue(item->data(Qt::UserRole + 1).toInt());
        error_->setVisible(false);
        password_->setFocus();   // 地址填好了，下一步本来就是输口令
    });

    auto *box = new QVBoxLayout(this);
    box->addWidget(peersLabel_);
    box->addWidget(peers_);
    box->addLayout(form);
    box->addWidget(buttons);
}

void ConnectDialog::attachDiscovery(Discovery *discovery)
{
    if (discovery_ == discovery)
        return;
    discovery_ = discovery;
    if (discovery_) {
        // this 作为上下文：对话框销毁时连接自动断开（Discovery 活得更久）。
        connect(discovery_, &Discovery::peersChanged, this, &ConnectDialog::refreshPeers);
    }
    refreshPeers();
}

void ConnectDialog::refreshPeers()
{
    if (!peers_)
        return;

    // 一行 = **一台机器的一条路**（地址:端口 · 机器名）。地址是给程序用的、
    // 名字是给人认的，分两行会把每条撑得又高又宽，一屏只看得到两台。
    //
    // 同一台机器接了 Wi-Fi 和网线时会有两行：两个地址都是能连过去的路，
    // 而哪条通只有用户知道（有线接内网、无线接外网时常见），所以**全都列出来**，
    // 点哪行就连哪个 IP。对齐用的键因此是「实例 + 地址」—— 按实例对齐会让
    // 同一台机器的两行抢同一个条目对象，看着就是两行在互相覆盖。
    constexpr int kAddressRole = Qt::UserRole;
    constexpr int kPortRole    = Qt::UserRole + 1;
    constexpr int kKeyRole     = Qt::UserRole + 2;

    const QList<DiscoveredPeer> list =
        discovery_ ? discovery_->peers() : QList<DiscoveredPeer>{};

    // 摊平成「一行一个地址」，顺序由 Discovery 给（名字 → 地址），不在这里再排。
    struct PeerRow
    {
        QString key;
        QString address;
        quint16 port = 0;
        QString name;
    };
    QList<PeerRow> rows;
    for (const DiscoveredPeer &peer : list) {
        if (peer.addresses.isEmpty())
            continue;   // 理论上不会：没有地址的条目不会进列表
        for (const PeerAddress &entry : peer.addresses) {
            const QString address = entry.address.toString();
            rows.append(PeerRow{QStringLiteral("%1@%2").arg(peer.instanceId).arg(address),
                                address, peer.port, peer.name});
        }
    }

    // 增量更新：**不 clear() 重建**。一重建，选中项、悬停项、滚动位置全丢，
    // 而且每次广播都闪一下，看着就是在抖。这里只改文本 / 挪位置 / 补缺 / 删多。
    for (int row = 0; row < rows.size(); ++row) {
        const PeerRow &peer = rows.at(row);

        QListWidgetItem *item = nullptr;
        for (int i = 0; i < peers_->count() && !item; ++i) {
            if (peers_->item(i)->data(kKeyRole).toString() == peer.key)
                item = peers_->item(i);
        }
        if (!item) {
            item = new QListWidgetItem;
            item->setData(kKeyRole, peer.key);
        }

        item->setText(peer.name.isEmpty()
                          ? QStringLiteral("%1:%2").arg(peer.address).arg(peer.port)
                          : QStringLiteral("%1:%2 · %3").arg(peer.address).arg(peer.port).arg(
                                peer.name));
        item->setData(kAddressRole, peer.address);
        item->setData(kPortRole, int(peer.port));

        const int current = peers_->row(item);
        if (current != row) {
            if (current >= 0)
                peers_->takeItem(current);
            peers_->insertItem(row, item);
        }
    }
    while (peers_->count() > rows.size())
        delete peers_->takeItem(peers_->count() - 1);

    const int visibleRows = qMin(peers_->count(), 4);
    peers_->setVisible(visibleRows > 0);
    peersLabel_->setVisible(visibleRows > 0);
    if (visibleRows > 0)
        peers_->setFixedHeight(visibleRows * peers_->sizeHintForRow(0)
                               + 2 * peers_->frameWidth());
}

ConnectDialog::Config ConnectDialog::config() const
{
    Config c;
    c.host     = host_->text().trimmed();
    c.port     = quint16(port_->value());
    c.password = password_->text();
    return c;
}

// 地址为空 / 口令不合规时都不关对话框 —— 关了也是必然失败的一次连接，不如当场说清楚。
// 口令规则与被控端相同（`app/PasswordPolicy.h`）：两端同版本时短口令在对端也一定会被拒，
// 本地先拦下能省一次无用的连接尝试。
void ConnectDialog::accept()
{
    if (host_->text().trimmed().isEmpty()) {
        // 与口令错误同一条红字、同一个位置：别用「改 placeholder」这种一次性提示 ——
        // 用户根本分不清它跟红字是不是同一件事，而且下一次打开窗口它还在。
        error_->setText(tr("请填写对方地址。"));
        error_->setVisible(true);
        host_->setFocus();
        return;
    }
    if (!isValidPassword(password_->text())) {
        error_->setText(
            tr("口令至少 6 位，且只能用英文字母、数字和符号。"));
        error_->setVisible(true);
        password_->setFocus();
        return;
    }
    QDialog::accept();
}

// ---------------------------------------------------------------------------
// 【设置…】
// ---------------------------------------------------------------------------

SettingsDialog::SettingsDialog(const Config &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("设置"));
    // ⚠️ **不设**最小宽度：这里的每一行都很短（最长的是底部那句），硬撑到启动
    // 对话框的 320 只会在右侧留一条空白。窗口宽度由布局自然算出 = 最长一行。

    autoReconnect_ = new QCheckBox(tr("断线自动重连"), this);
    autoReconnect_->setObjectName(QStringLiteral("autoReconnectCheck"));
    autoReconnect_->setChecked(initial.autoReconnect);

    audio_ = new QCheckBox(tr("接收对方声音"), this);
    audio_->setObjectName(QStringLiteral("audioCheck"));
    audio_->setChecked(initial.receiveAudio);

    tray_ = new QCheckBox(tr("最小化到托盘"), this);
    tray_->setObjectName(QStringLiteral("trayResidentCheck"));
    tray_->setChecked(initial.trayResident);

    singleInstance_ = new QCheckBox(tr("同时只开一个窗口"), this);
    singleInstance_->setObjectName(QStringLiteral("singleInstanceCheck"));
    singleInstance_->setChecked(initial.singleInstance);

    autoStart_ = new QCheckBox(tr("开机后自动开始等待连接"), this);
    autoStart_->setObjectName(QStringLiteral("autoStartCheck"));
    autoStart_->setChecked(initial.autoStart);

    input_ = new QCheckBox(tr("键鼠控制"), this);
    input_->setObjectName(QStringLiteral("inputCheck"));
    input_->setChecked(initial.inputEnabled);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // 六个开关里只有「键鼠控制」不记住，这句是唯一必须留在界面上的说明 ——
    // 它不是对某个选项的解释，而是对「例外」的声明：不说，用户会以为下次开程序
    // 自己昨天勾过的操作权限还在。
    auto *footnote = new QLabel(tr("以上设置会自动保存，键鼠控制除外。"), this);
    footnote->setObjectName(QStringLiteral("formFootnote"));
    // 不换行：换行会让人以为那是两段说明，也会让 QLabel 的 sizeHint 变得不可预测
    // （窗口宽度要按「最长一行」算，就得让它老实报出这一行的真实宽度）。
    footnote->setWordWrap(false);

    auto *box = new QVBoxLayout(this);
    // 每个开关后面**不再跟一行说明**：复选框上的文字本身就是说明
    //（「断开后按 1–30 秒递增间隔重试」这种解释在勾选前没人看，勾选后也会让人
    //  误以为那行是某个状态的实时描述）。真正需要交代的只有「哪一项不会被记住」，
    // 那是底部那句，它留着的理由与上面相反 —— 它不是解释控件，是例外声明。
    box->setSpacing(10);
    box->addWidget(autoReconnect_);
    box->addWidget(audio_);
    box->addWidget(tray_);
    box->addWidget(singleInstance_);
    box->addWidget(autoStart_);
    box->addWidget(input_);
    box->addSpacing(4);
    box->addWidget(footnote);
    box->addWidget(buttons);
}

SettingsDialog::Config SettingsDialog::config() const
{
    Config c;
    c.autoReconnect  = autoReconnect_->isChecked();
    c.receiveAudio   = audio_->isChecked();
    c.trayResident   = tray_->isChecked();
    c.singleInstance = singleInstance_->isChecked();
    c.autoStart      = autoStart_->isChecked();
    c.inputEnabled   = input_->isChecked();
    return c;
}

} // namespace lanrdp
