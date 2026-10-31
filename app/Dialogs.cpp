#include "app/Dialogs.h"

#include "app/PasswordEdit.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>

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

} // namespace

// ---------------------------------------------------------------------------
// 【开始监听…】
// ---------------------------------------------------------------------------

HostConfigDialog::HostConfigDialog(const Config &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("开始监听"));
    setMinimumWidth(kDialogMinWidth);

    auto *form = new QFormLayout();
    form->setVerticalSpacing(8);
    form->setContentsMargins(0, 0, 0, 0);

    port_ = new QSpinBox(this);
    port_->setObjectName(QStringLiteral("portEdit"));
    port_->setRange(kMinPort, kMaxPort);
    port_->setValue(int(initial.port));
    port_->setToolTip(QStringLiteral("通常无需修改。若提示端口被占用，请更换为 1024 以上的其他端口。"));

    password_ = new PasswordEdit(this);
    password_->setText(initial.password);
    password_->setObjectName(QStringLiteral("passwordEdit"));
    password_->setPlaceholderText(QStringLiteral("至少 6 位"));
    password_->setToolTip(QStringLiteral("对方连接本机时必须输入相同的访问口令。"));

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
    recvDir_->setToolTip(QStringLiteral(
        "对方发送的文件将保存到此目录（不存在时自动创建）。\n"
        "文件重名时自动追加 -1、-2 等后缀，不会覆盖已有文件。"));
    auto *browse = new QPushButton(QStringLiteral("浏览…"), this);
    browse->setObjectName(QStringLiteral("browseRecvDirButton"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString picked = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择接收目录"), recvDir_->text());
        if (!picked.isEmpty())
            recvDir_->setText(picked);
    });
    auto *dirRow = new QHBoxLayout();
    dirRow->setContentsMargins(0, 0, 0, 0);
    dirRow->addWidget(recvDir_, 1);
    dirRow->addWidget(browse);

    h264_ = new QCheckBox(QStringLiteral("压缩画面传输（推荐）"), this);
    h264_->setObjectName(QStringLiteral("h264Check"));
    h264_->setChecked(initial.useH264);
    h264_->setToolTip(QStringLiteral(
        "开启后画面先压缩再传输，可显著降低带宽占用，网络不佳时更流畅。\n"
        "仅排查问题时需要关闭。"));

    form->addRow(QStringLiteral("监听端口"), port_);
    form->addRow(QStringLiteral("访问口令"), password_);
    form->addRow(passwordError_);   // 横跨两列（挤在字段列里会断在词中间）；平时隐藏
    form->addRow(QStringLiteral("接收目录"), dirRow);
    form->addRow(h264_);   // 横跨两列

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
    firewallHint->setText(QStringLiteral("若连接不上，请在 Windows 防火墙中放行本程序。"));
    form->addRow(firewallHint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("开始监听"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    // 校验在 accept() 覆写里（唯一入口：点按钮和直接调 accept() 走同一条路）。
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // 一旦开始改就把上一轮的报错收起来（否则改对了红字还杵在那儿）。
    connect(password_, &QLineEdit::textChanged, this, [this](const QString &) {
        passwordError_->setVisible(false);
    });

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
    return c;
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
            QStringLiteral("口令至少 6 位，且只能用英文字母、数字和符号。"));
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
    setWindowTitle(QStringLiteral("新连接"));
    setMinimumWidth(kDialogMinWidth);   // 与【开始监听…】同宽，来回切换时窗口不跳

    auto *form = new QFormLayout();
    form->setVerticalSpacing(8);
    form->setContentsMargins(0, 0, 0, 0);

    host_ = new QLineEdit(initial.host, this);
    host_->setObjectName(QStringLiteral("peerHostEdit"));
    host_->setToolTip(QStringLiteral("远程电脑的 IP 地址或主机名。"));

    port_ = new QSpinBox(this);
    port_->setObjectName(QStringLiteral("peerPortEdit"));
    port_->setRange(kMinPort, kMaxPort);
    port_->setValue(int(initial.port));

    password_ = new PasswordEdit(this);
    password_->setText(initial.password);
    password_->setObjectName(QStringLiteral("connectPasswordEdit"));
    password_->setPlaceholderText(QStringLiteral("至少 6 位"));

    // 与【开始监听…】同款的红字：平时隐藏，点【连接】才校验。
    // 地址与口令共用这一条 —— 两类错误都属于「这一栏没填对」，不该用两种呈现方式。
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("connectErrorHint"));
    error_->setWordWrap(true);
    error_->setVisible(false);

    form->addRow(QStringLiteral("对方地址"), host_);
    form->addRow(QStringLiteral("对方端口"), port_);
    form->addRow(QStringLiteral("访问口令"), password_);
    form->addRow(error_);   // 横跨两列；平时隐藏

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("连接"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
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

    auto *box = new QVBoxLayout(this);
    box->addLayout(form);
    box->addWidget(buttons);
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
        error_->setText(QStringLiteral("请填写对方地址。"));
        error_->setVisible(true);
        host_->setFocus();
        return;
    }
    if (!isValidPassword(password_->text())) {
        error_->setText(
            QStringLiteral("口令至少 6 位，且只能用英文字母、数字和符号。"));
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
    setWindowTitle(QStringLiteral("设置"));

    autoReconnect_ = new QCheckBox(QStringLiteral("断线自动重连"), this);
    autoReconnect_->setObjectName(QStringLiteral("autoReconnectCheck"));
    autoReconnect_->setChecked(initial.autoReconnect);
    autoReconnect_->setToolTip(QStringLiteral(
        "连接意外断开后自动重试，重连间隔从 1 秒逐步延长至 30 秒。\n"
        "点击【断开】或取消勾选后不再重连。"));

    input_ = new QCheckBox(QStringLiteral("启用远程键鼠控制"), this);
    input_->setObjectName(QStringLiteral("inputCheck"));
    input_->setChecked(initial.inputEnabled);
    input_->setToolTip(QStringLiteral(
        "开启后，本机的鼠标与键盘操作将发送至对方电脑。\n"
        "需先点击画面取得焦点，键盘操作方可生效。\n"
        "关闭后，本机操作不影响对方。"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *hint = new QLabel(QStringLiteral(
        "以上设置仅本次运行有效，退出程序后不保留。"), this);
    hint->setObjectName(QStringLiteral("formHint"));
    hint->setWordWrap(true);

    auto *box = new QVBoxLayout(this);
    box->addWidget(autoReconnect_);
    box->addWidget(input_);
    box->addWidget(hint);
    box->addWidget(buttons);
}

SettingsDialog::Config SettingsDialog::config() const
{
    Config c;
    c.autoReconnect = autoReconnect_->isChecked();
    c.inputEnabled  = input_->isChecked();
    return c;
}

} // namespace lanrdp
