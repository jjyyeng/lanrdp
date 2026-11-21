#pragma once

#include <QString>
#include <QWidget>

namespace lanrdp {

// 隐私遮挡的**本机那一层**：一块铺满整个虚拟桌面的纯黑窗。
//
// 三件刻意这么做的事：
// · 盖的是**虚拟桌面的并集**而不是主屏 —— 双屏下只盖一块等于没盖，
//   而「另一半屏幕上还留着刚才的窗口」正是这个功能要防的东西。
// · `Qt::Tool` + 永不进任务栏：遮挡期间不该多出一条能被外人点开的窗口。
// · 一定给一行逃生提示（热键 + 点一下）。纯黑又忘了热键 = 把机主自己锁死；
//   Windows 锁屏至少还有一行字。
//
// ⚠️ 它不碰 DXGI 输出，也不负责暂停推流 —— 那是 `HostController::setPrivacyCurtain`
// 的事。这里只管「给人看的那一层」，两件事分开才可能各自被断言。
class PrivacyCurtain : public QWidget
{
    Q_OBJECT

public:
    explicit PrivacyCurtain(QWidget *parent = nullptr);

    // 铺满整个虚拟桌面并置于最前。重复调用是幂等的。
    void showOverVirtualDesktop();

    // 逃生提示里那句热键文本（"Ctrl+Alt+F11"）。随语言 / 改键刷新。
    void setHotkeyText(const QString &text);
    QString hotkeyText() const { return hotkeyText_; }

signals:
    // 用户点了一下或按了 Esc。**隐藏由调用方做** —— 遮挡是「一边黑窗 + 一边停流」
    // 的两件事，只关窗口会让推流和操作权留在暂停状态。
    void dismissed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    QString hotkeyText_;
};

} // namespace lanrdp
