#pragma once

#include <mutex>
#include <memory>
#include <string>

#include "noncopyable.h"
#include "TcpConnection.h"

class Connector;
class EventLoop;
class InetAddress;

// 客户端：Connector 建连后装配 TcpConnection，回调风格与 TcpServer 一致。
class TcpClient : noncopyable
{
public:
    TcpClient(EventLoop *loop, const InetAddress &serverAddr, const std::string &nameArg);
    ~TcpClient();

    void connect();
    void disconnect();
    void stop();

    TcpConnectionPtr connection() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return connection_;
    }

    EventLoop *getLoop() const { return loop_; }
    bool retry() const { return retry_; }
    void enableRetry() { retry_ = true; }

    void setConnectionCallback(const ConnectionCallback &cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback &cb) { writeCompleteCallback_ = cb; }

private:
    void newConnection(int sockfd);
    void removeConnection(const TcpConnectionPtr &conn);

    EventLoop *loop_;
    std::shared_ptr<Connector> connector_;
    const std::string name_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    bool retry_;
    bool connect_;
    int nextConnId_;
    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;
};
