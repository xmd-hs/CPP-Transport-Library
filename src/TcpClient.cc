#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "TcpClient.h"
#include "Connector.h"
#include "EventLoop.h"
#include "Logger.h"

namespace
{

EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d loop is null!\n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

} // namespace

TcpClient::TcpClient(EventLoop *loop, const InetAddress &serverAddr, const std::string &nameArg)
    : loop_(CheckLoopNotNull(loop))
    , connector_(std::make_shared<Connector>(loop, serverAddr))
    , name_(nameArg)
    , connectionCallback_()
    , messageCallback_()
    , retry_(false)
    , connect_(false)
    , nextConnId_(1)
{
    connector_->setNewConnectionCallback(
        std::bind(&TcpClient::newConnection, this, std::placeholders::_1));
}

TcpClient::~TcpClient()
{
    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }

    if (conn)
    {
        conn->setCloseCallback([](const TcpConnectionPtr &c) {
            c->getLoop()->queueInLoop(std::bind(&TcpConnection::connectDestroyed, c));
        });
        conn->forceClose();
    }
    else
    {
        connector_->stop();
    }
}

void TcpClient::connect()
{
    connect_ = true;
    connector_->start();
}

void TcpClient::disconnect()
{
    connect_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_)
        {
            connection_->shutdown();
        }
    }
}

void TcpClient::stop()
{
    connect_ = false;
    connector_->stop();
}

void TcpClient::newConnection(int sockfd)
{
    sockaddr_in peer;
    ::memset(&peer, 0, sizeof(peer));
    socklen_t len = sizeof(peer);
    if (::getpeername(sockfd, reinterpret_cast<sockaddr *>(&peer), &len) < 0)
    {
        LOG_ERROR("TcpClient::newConnection getpeername");
    }
    InetAddress peerAddr(peer);

    sockaddr_in local;
    ::memset(&local, 0, sizeof(local));
    len = sizeof(local);
    if (::getsockname(sockfd, reinterpret_cast<sockaddr *>(&local), &len) < 0)
    {
        LOG_ERROR("TcpClient::newConnection getsockname");
    }
    InetAddress localAddr(local);

    char buf[64];
    snprintf(buf, sizeof(buf), "-%s#%d", peerAddr.toIpPort().c_str(), nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;

    TcpConnectionPtr conn =
        makePoolShared<TcpConnection>(loop_, connName, sockfd, localAddr, peerAddr);
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(
        std::bind(&TcpClient::removeConnection, this, std::placeholders::_1));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;
    }
    conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr &conn)
{
    loop_->runInLoop([this, conn]() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (connection_ == conn)
            {
                connection_.reset();
            }
        }
        loop_->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));

        if (retry_ && connect_)
        {
            connector_->restart();
        }
    });
}
