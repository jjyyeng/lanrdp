#pragma once

#include <QLineEdit>

class QResizeEvent;
class QToolButton;

namespace lanrdp {

// 口令输入框：右侧**内嵌**「显示 / 隐藏」开关。
//
// 为什么嵌进去而不是在旁边并排：并排要多吃掉一截字段列宽度（两个启动对话框都只有
// 320px 起），而这个开关本来就该待在光标旁边 —— 用户点它的时候眼睛正看着输入框。
// 代价是自己摆子控件的位置（见 placeToggle）：QLineEdit 只支持 addAction(图标)，
// 不支持塞一个真控件，所以只能这么干。
//
// ⚠️ 状态规则：默认隐藏；切换**只改 echoMode()**，`text()` 不变（校验、落盘读的都是
// text，完全不用知道有这个开关）；每次新建对象都从隐藏开始 —— 明文状态不记忆、
// 不落盘、不进日志。
class PasswordEdit : public QLineEdit
{
    Q_OBJECT

public:
    explicit PasswordEdit(QWidget *parent = nullptr);

    bool isPasswordVisible() const { return echoMode() == QLineEdit::Normal; }

public slots:
    void setPasswordVisible(bool visible);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void syncToggle();
    // 把开关摆到输入框内右侧，并把文字区域让出来。
    void placeToggle();

    QToolButton *toggle_ = nullptr;
};

} // namespace lanrdp
