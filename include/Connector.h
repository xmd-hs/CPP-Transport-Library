#pragma once

#include <functional>
#include <memory>

#include "InetAddress.h"
#include "TimerId.h"
#include "noncopyable.h"

class Channel;
class EventLoop;

// 非阻塞 connect；失败后指数退避重试（依赖 EventLoop 定时器）。
class Connector : noncopyable, public std::enable_shared_from_this<Connector>
{
public:
    using NewConnectionCallback = std::function<void(int sockfd)>;

    Connector(EventLoop *loop, const InetAddress &serverAddr);
    ~Connector();

    void setNewConnectionCallback(const NewConnectionCallback &cb) { newConnectionCallback_ = cb; }

    void start();
    void stop();
    void restart();

    const InetAddress &serverAddress() const { return serverAddr_; }

private:
    enum States
    {
        kDisconnected,
        kConnecting,
        kConnected
    };

    static const int kMaxRetryDelayMs;
    static const int kInitRetryDelayMs;

    void setState(States s) { state_ = s; }
    void startInLoop();
    void stopInLoop();
    void connect();
    void connecting(int sockfd);
    void handleWrite();
    void handleError();
    void retry(int sockfd);
    void resetChannel();
    int removeAndResetChannel();

    EventLoop *loop_;
    InetAddress serverAddr_;
    bool connect_;
    States state_;
    std::unique_ptr<Channel> channel_;
    NewConnectionCallback newConnectionCallback_;
    int retryDelayMs_;
    TimerId timerId_;
};
