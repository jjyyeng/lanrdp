#pragma once

#include "app/PasswordPolicy.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace lanrdp {

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
    };

    explicit HostConfigDialog(const Config &initial, QWidget *parent = nullptr);

    // 只在 exec() 返回 Accepted 之后读才有意义。
    Config config() const;

    // 口令不合规（空 / 太短 / 含非 ASCII）时**不关窗口**，在口令框下方说明原因。
    // 空口令尤其要拦：两端 sha256("") 一致，握手**会通过** —— 那不是「连不上」，
    // 而是「谁都能连」，被控端会退化成没有凭据的远程键鼠服务。
    // 规则在 app/PasswordPolicy.h（与命令行共用同一个判定口）。
    // 保持 public：基类 QDialog::accept() 就是 public slot，覆写时收窄访问会造成
    // 「同一个调用在 QDialog* 上能过、在 HostConfigDialog 上却编不过」的怪现象。
    void accept() override;

private:
    QSpinBox  *port_          = nullptr;
    QLineEdit *password_      = nullptr;
    QLabel    *passwordError_ = nullptr;
    QLineEdit *recvDir_       = nullptr;
    QCheckBox *h264_          = nullptr;
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
    QLineEdit *host_     = nullptr;
    QSpinBox  *port_     = nullptr;
    QLineEdit *password_ = nullptr;
    // 地址为空 / 口令不合规共用**同一条**红字：两类错误都是「这一栏没填对」，
    // 用两套提示（一个改 placeholder、一个常驻红字）会让人以为是两种不同的问题。
    QLabel    *error_    = nullptr;
};

// 【设置…】：**本次运行内**的全局开关（不落盘、不跨会话，退出即恢复默认）。
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    struct Config {
        bool autoReconnect = true;   // 断线自动重连
        bool inputEnabled  = false;  // 启用远程键鼠控制
    };

    explicit SettingsDialog(const Config &initial, QWidget *parent = nullptr);

    Config config() const;

private:
    QCheckBox *autoReconnect_ = nullptr;
    QCheckBox *input_         = nullptr;
};

} // namespace lanrdp
