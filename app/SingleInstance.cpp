#include "app/SingleInstance.h"

#include <QLocalServer>
#include <QLocalSocket>

namespace lanrdp {

SingleInstanceGuard::SingleInstanceGuard(QObject *parent) : QObject(parent) {}

SingleInstanceGuard::~SingleInstanceGuard() = default;

QString SingleInstanceGuard::key() const
{
    return key_;
}

bool SingleInstanceGuard::listen(const QString &key)
{
    // 重复调用先收掉旧监听：一个 guard 只对应一条管道。否则旧对象仍在听同一个
    // 名字（Windows 允许同名多实例），而 stop() 只关得掉最新那个 —— 结果是
    // 「取消勾选」之后名字没真放掉，新实例照样被拦。close() 是同步的，名字当场释放。
    stop();

    key_ = key;

    // 清掉残留的监听名（Unix 上 socket 文件会留在文件系统里；Windows 命名
    // 管道随进程消亡，这一步是空操作）。清不掉也不拦 —— listen 会如实失败。
    QLocalServer::removeServer(key);

    server_ = new QLocalServer(this);
    if (!server_->listen(key)) {
        // 建不起来（名字被占、权限不够）：fail-open —— 放手照常启动。
        server_->deleteLater();
        server_ = nullptr;
        return false;
    }

    // 连接本身就是唤醒信号，不传数据。曾经试过「客户端写一条 raise 消息、
    // 服务端收到后 emit」：Windows 命名管道上客户端写缓冲要靠事件循环排空
    // （waitForBytesWritten 实测恒 false），同步等待根本送不出数据，消息
    // 随连接一起被丢 —— 而连接建立这件事 waitForConnected 就能同步确认。
    connect(server_, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *conn = server_->nextPendingConnection()) {
            conn->disconnectFromServer();
            connect(conn, &QLocalSocket::disconnected, conn, &QLocalSocket::deleteLater);
            emit raiseRequested();
        }
    });
    return true;
}

void SingleInstanceGuard::stop()
{
    if (!server_)
        return;
    server_->close();
    server_->deleteLater();
    server_ = nullptr;
}

bool SingleInstanceGuard::notifyRunning(const QString &key)
{
    QLocalSocket socket;
    socket.connectToServer(key);
    // 连上了 = 有实例在听，唤醒意图由连接本身传达（见 listen 里的说明），
    // 对端会在事件循环里收到并把自己叫到前台。
    if (!socket.waitForConnected(300))
        return false;
    socket.disconnectFromServer();
    return true;
}

} // namespace lanrdp
