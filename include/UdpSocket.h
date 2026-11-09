#pragma once

#include <functional>
#include <memory>
#include <string>

#include "InetAddress.h"
#include "noncopyable.h"

class EventLoop;
class Channel;

// 非阻塞 UDP：bind + Channel 读就绪时排空 recvFrom。
class UdpSocket : noncopyable
{
public:
    using MessageCallback =
        std::function<void(const char *data, size_t len, const InetAddress &peer)>;

    UdpSocket(EventLoop *loop, const InetAddress &localAddr);
    ~UdpSocket();

    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }
    void start();
    void stop();

    ssize_t sendTo(const void *data, size_t len, const InetAddress &peer);
    int fd() const { return sockfd_; }
    const InetAddress &localAddress() const { return localAddr_; }

private:
    void handleRead();

    EventLoop *loop_;
    int sockfd_;
    InetAddress localAddr_;
    std::unique_ptr<Channel> channel_;
    MessageCallback messageCallback_;
    bool started_;
};
