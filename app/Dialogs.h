#pragma once

#include "app/PasswordPolicy.h"

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace lanrdp {

class PasswordEdit;
class Discovery;

// 口令默认**留空**，由用户当场输入：一条公开写在源码里的出厂口令等于没设。
// 长度与字符集的规则在 app/PasswordPolicy.h（唯一判定口）。
//
// 被控端默认接收目录 —— **绝对路径**（系统「下载」目录；取不到时退回 exe 同目录的 received）。
// ⚠️ 不能默认成相对路径 "received"：它按**进程当前工作目录**解析，而程序由 Explorer /
// 快捷方式启动时 CWD 不保证是 exe 所在目录，收到的文件会散落到意外位置。
// 写在 Dialogs.cpp 里（而非各处的字面量），是为了「对话框默认值」与「MainWindow 的兜底」
// 永远指向同一个地方。
QString defaultReceiveDir();

// ---------------------------------------------------------------------------
// 三个对话框。
//
// 分工的判据是「这个参数属于哪一层」：
// · 【开始监听…】/【新连接…】 = **启动一次会话**的参数（端口 / 口令 / 目录 / 编码）。
// 它们是"启动前"的东西，用完就藏进对话框，不占界面。
// · 【设置…】 = **本次运行内**对所有画面生效的开关（自动重连 / 键鼠总开关）。
//
// ⚠️ 【设置…】刻意**不放**端口 / 口令 / 接收目录：那些在启动对话框里。
// 同一个状态有两处可编辑，就必然有一处会忘记同步 —— 与
// 「少存一份就少一处能不同步的地方」的同一套取舍。
// ---------------------------------------------------------------------------

// 【开始监听…】：被控端启动参数。
class HostConfigDialog : public QDialog
{
    Q_OBJECT

public:
    struct Config {
        quint16 port        = 9000;
        QString password;                       // 空 = 必须由用户当场输入
        QString receiveDir  = defaultReceiveDir();
        bool    useH264     = true;
        // 压缩传输的码率上限（bps）。自适应只在这个值**以下**调节，所以它是天花板
        // 而非固定值：设 16 Mbps 不代表一定发 16 Mbps，网络差时会自己降。
        int     bitrateBps  = 8 * 1000 * 1000;
    };

    explicit HostConfigDialog(const Config &initial, QWidget *parent = nullptr);

    // 只在 exec() 返回 Accepted 之后读才有意义。
    Config config() const;

    // 【复制邀请串】要复制的内容：地址 / 端口 / 口令 三行（拼接规则见
    // common/LanAddresses.h::inviteText）。**按当前输入框的实时值**算。
    //
    // 公开出来是为了能脱离剪贴板断言 —— offscreen 平台上 QClipboard 不可靠，
    // 而这段文本对不对，比「能不能复制」更值得钉。
    QString inviteString() const;

    // 口令不合规（空 / 太短 / 含非 ASCII）时**不关窗口**，在口令框下方说明原因。
    // 空口令尤其要拦：两端 sha256("") 一致，握手**会通过** —— 那不是「连不上」，
    // 而是「谁都能连」，被控端会退化成没有凭据的远程键鼠服务。
    // 规则在 app/PasswordPolicy.h（与命令行共用同一个判定口）。
    // 保持 public：基类 QDialog::accept() 就是 public slot，覆写时收窄访问会造成
    // 「同一个调用在 QDialog* 上能过、在 HostConfigDialog 上却编不过」的怪现象。
    void accept() override;

private:
    // 口令不合规就不复制：邀请串的核心是口令，缺了它对方拿到文本照样连不上。
    void copyInvite();

    QSpinBox  *port_          = nullptr;
    PasswordEdit *password_   = nullptr;
    QLabel    *passwordError_ = nullptr;
    QLineEdit *recvDir_       = nullptr;
    QCheckBox *h264_          = nullptr;
    QComboBox *bitrate_       = nullptr;
    QPushButton *invite_      = nullptr;
    // 构造时枚举一次：邀请串里的地址行用它，按钮可用性也用它。
    QStringList lanAddresses_;
};

// 【新连接…】：控制端连接参数。
class ConnectDialog : public QDialog
{
    Q_OBJECT

public:
    struct Config {
        QString host      = QStringLiteral("127.0.0.1");
        quint16 port      = 9000;
        QString password;   // 空 = 必须由用户当场输入
    };

    explicit ConnectDialog(const Config &initial, QWidget *parent = nullptr);

    Config config() const;

    // 挂上自动发现：对话框打开期间新发现的机器会实时出现在顶部列表里。
    // 传 nullptr / 一条都没有 → 整块列表隐藏（不留一个空框占地方）。
    // ⚠️ 只填空地址与端口：口令永远要人手输 —— 广播里没有口令（也不该有）。
    void attachDiscovery(Discovery *discovery);

    // 地址为空 / 口令不合规时都不关窗口（关了也必然连不上，不如当场说清楚）。
    // 口令规则与被控端相同（`app/PasswordPolicy.h`）—— 两端同版本时，短口令
    // 在对端也一定会被拒，所以本地先拦下省一次无用的连接尝试。
    // ⚠️ 只拦在**对话框**这一层：`--connect` 与 `startConnect()` 不校验，
    // 那是自动化入口，口令必须能跟着被测端走。
    // 保持 public：基类 QDialog::accept() 就是 public slot，覆写时收窄访问会造成
    // 「同一个调用在 QDialog* 上能过、在 ConnectDialog 上却编不过」的怪现象
    //（对标同文件 HostConfigDialog::accept() 的写法）。
    void accept() override;

private:
    void refreshPeers();

    QLineEdit *host_     = nullptr;
    QSpinBox  *port_     = nullptr;
    PasswordEdit *password_   = nullptr;
    // 地址为空 / 口令不合规共用**同一条**红字：两类错误都是「这一栏没填对」，
    // 用两套提示（一个改 placeholder、一个常驻红字）会让人以为是两种不同的问题。
    QLabel    *error_    = nullptr;

    // ---- 自动发现（顶部那一块）----
    QLabel      *peersLabel_ = nullptr;
    QListWidget *peers_      = nullptr;
    Discovery   *discovery_  = nullptr;   // 不拥有
};

// 【设置…】：连接、窗口与控制选项的唯一编辑处。
//
// 其中五项会被记住（落盘在 `app/` 下）；「键鼠控制」每次启动都要重新开启 ——
// 那是把本机操作发到别人机器上的能力，不该默认继承上一次的同意。
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    struct Config {
        bool autoReconnect  = true;   // 断线自动重连
        bool receiveAudio   = true;   // 接收对方声音
        bool trayResident   = false;  // 最小化到托盘
        bool singleInstance = false;  // 同时只开一个窗口
        // 开机自启 + 启动即监听合成一个开关：两者分开没有价值 —— 不自启时
        // 「启动即监听」只是省一次点击，却会在双开 A/B 互连时多一步去关。
        bool autoStart      = false;
        bool inputEnabled   = false;  // 键鼠控制（仅本次运行）
    };

    explicit SettingsDialog(const Config &initial, QWidget *parent = nullptr);

    Config config() const;

private:
    QCheckBox *autoReconnect_  = nullptr;
    QCheckBox *audio_          = nullptr;
    QCheckBox *tray_           = nullptr;
    QCheckBox *singleInstance_ = nullptr;
    QCheckBox *autoStart_      = nullptr;
    QCheckBox *input_          = nullptr;
};

} // namespace lanrdp
