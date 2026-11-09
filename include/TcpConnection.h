#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <chrono>

#include "noncopyable.h"
#include "InetAddress.h"
#include "Callbacks.h"
#include "Buffer.h"
#include "Timestamp.h"
#include "KamaMemory.h"

class Channel;
class EventLoop;
class Socket;

/**
 * TcpServer => Acceptor => 有一个新用户连接，通过accept函数拿到connfd
 * => TcpConnection设置回调 => 设置到Channel => Poller => Channel回调
 **/

class TcpConnection : noncopyable, public std::enable_shared_from_this<TcpConnection>
{
public:
    TcpConnection(EventLoop *loop,
                  const std::string &nameArg,
                  int sockfd,
                  const InetAddress &localAddr,
                  const InetAddress &peerAddr);
    ~TcpConnection();

    EventLoop *getLoop() const { return loop_; }
    const std::string &name() const { return name_; }
    const InetAddress &localAddress() const { return localAddr_; }
    const InetAddress &peerAddress() const { return peerAddr_; }

    bool connected() const { return state_ == kConnected; }
    bool isWriting() const;
    bool isSendingFile() const { return sendFileRemaining_ > 0; }
    // Access on the connection's loop thread, like outputBufferBytes().
    std::chrono::steady_clock::time_point lastFileWriteTime() const { return lastFileWriteTime_; }
    size_t outputBufferBytes() const { return outputBuffer_.readableBytes(); }
    size_t highWaterMark() const { return highWaterMark_; }

    // 发送数据。同线程直接写，避免先拷成 string。
    void send(const std::string &buf);
    void send(const void *data, size_t len);
    // Owns a duplicate until completion. One file send may be in flight per connection.
    void sendFile(int fileDescriptor, off_t offset, size_t count);

    // 关闭半连接
    void shutdown();
    // 立即关闭连接（可跨线程）
    void forceClose();

    // 业务侧挂接任意上下文（如 TransferSession），避免全局 map。
    void setContext(const std::shared_ptr<void> &context) { context_ = context; }
    std::shared_ptr<void> getContext() const { return context_; }
    template <typename T>
    std::shared_ptr<T> getContext() const
    {
        return std::static_pointer_cast<T>(context_);
    }

    void setConnectionCallback(const ConnectionCallback &cb)
    { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb)
    { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback &cb)
    { writeCompleteCallback_ = cb; }
    void setCloseCallback(const CloseCallback &cb)
    { closeCallback_ = cb; }
    void setHighWaterMarkCallback(const HighWaterMarkCallback &cb, size_t highWaterMark)
    { highWaterMarkCallback_ = cb; highWaterMark_ = highWaterMark; }

    // 连接建立
    void connectEstablished();
    // 连接销毁
    void connectDestroyed();

private:
    enum StateE
    {
        kDisconnected, // 已经断开连接
        kConnecting,   // 正在连接
        kConnected,    // 已连接
        kDisconnecting // 正在断开连接
    };
    void setState(StateE state) { state_ = state; }

    void handleRead(Timestamp receiveTime);
    void handleWrite();//处理写事件
    void handleClose();
    void handleError();

    void sendInLoop(const void *data, size_t len);
    // 跨线程发送时，把数据的所有权留在回调里，避免调用方缓冲区先被释放。
    void sendOwnedInLoop(const std::shared_ptr<PoolString> &message);
    void shutdownInLoop();
    void forceCloseInLoop();
    void sendFileInLoop(int fileDescriptor, off_t offset, size_t count);
    void scheduleWriteComplete();
    void invokeWriteComplete();
    EventLoop *loop_; // 这里是baseloop还是subloop由TcpServer中创建的线程数决定 若为多Reactor 该loop_指向subloop 若为单Reactor 该loop_指向baseloop
    const std::string name_;
    std::atomic_int state_;
    bool reading_;//连接是否在监听读事件

    // Socket Channel 这里和Acceptor类似    Acceptor => mainloop    TcpConnection => subloop
    PoolUniquePtr<Socket> socket_;
    PoolUniquePtr<Channel> channel_;

    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    // 这些回调TcpServer也有 用户通过写入TcpServer注册 TcpServer再将注册的回调传递给TcpConnection TcpConnection再将回调注册到Channel中
    ConnectionCallback connectionCallback_;       // 有新连接时的回调
    MessageCallback messageCallback_;             // 有读写消息时的回调
    WriteCompleteCallback writeCompleteCallback_; // 消息发送完成以后的回调
    HighWaterMarkCallback highWaterMarkCallback_; // 高水位回调
    CloseCallback closeCallback_; // 关闭连接的回调
    size_t highWaterMark_; // 高水位阈值

    // sendfile 没写完时记下进度，等 EPOLLOUT 再继续，避免在循环里空转。
    int sendFileFd_;
    struct FileOwner;
    std::shared_ptr<FileOwner> sendFileOwner_;
    off_t sendFileOffset_;
    size_t sendFileRemaining_;
    std::chrono::steady_clock::time_point lastFileWriteTime_;
    bool writeCompleteScheduled_; // 合并 writeComplete，避免推流时回调风暴

    std::shared_ptr<void> context_;

    // 数据缓冲区
    Buffer inputBuffer_;    // 接收数据的缓冲区
    Buffer outputBuffer_;   // 发送数据的缓冲区 用户send向outputBuffer_发
};
