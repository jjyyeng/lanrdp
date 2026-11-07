#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace lanrdp {

// 本程序的单实例监听名。探测与监听共用一个出处，两处写两份早晚会漂移。
inline QString singleInstanceKey()
{
    return QStringLiteral("lanrdp-instance-v1");
}

// 「只允许运行一个实例」的协调器（可选，默认关）。
//
// 原则是 fail-open：任何环节失败 —— 建不了监听、连不上对端、读到半截消息 ——
// 后果一律是「多开一个」或「少叫一次人」，绝不能反过来把启动拦掉。
// 从前用 QLockFile 做过一版：锁文件建不出来时程序会完全打不开，被整条撤掉。
// 命名管道随进程消亡，没有锁文件残留这类问题，兜底方向天然是对的。
//
// 判定语义：新实例要不要退出，取决于**已有实例**有没有开单实例 —— 互斥
// 本来就不需要被拦的一方同意。所以探测是无条件做的；这里的开关只决定
// 「我要不要当那个唯一实例」（见 kSingleInstanceKey 的使用方）。
//
// ⚠️ Windows 命名管道同名多实例本来就合法：两个进程对同一个 key listen
// 都会"成功"，但消息只会落进其中之一。真正的防重不靠 listen 互斥，靠的是
// 使用顺序 —— 新实例先 notifyRunning 探测、探测到了就不该再 listen。
// 毫秒级竞态窗口里绕过顺序的结果是两个都活着（fail-open 方向，可接受）。
class SingleInstanceGuard : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstanceGuard(QObject *parent = nullptr);
    ~SingleInstanceGuard() override;

    // 开始监听。返回是否真的建起来了；失败时调用方照常启动，只是失去
    // 「被后来的实例叫醒」的能力。
    bool listen(const QString &key);

    // 停止监听、交出名字。之后新实例不会被拦。
    void stop();

    // 新实例用：已有实例在听就发一条唤醒并返回 true（调用方随即退出）。
    // 连不上 —— 没人听、key 残留、跨完整性级别被拒 —— 一律返回 false，
    // 调用方照常启动。
    static bool notifyRunning(const QString &key);

    QString key() const;

signals:
    // 有别的实例连上来了（连上即唤醒，不传数据 —— 见实现里的说明）。
    void raiseRequested();

private:
    QLocalServer *server_ = nullptr;
    QString       key_;
};

} // namespace lanrdp
