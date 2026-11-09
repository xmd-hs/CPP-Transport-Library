#include <functional>
#include <algorithm>
#include <string>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <fcntl.h> // for open
#include <unistd.h> // for close

#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

// allocate_shared keeps the descriptor owner and reference count in one pool
// allocation. The descriptor remains owned across queued calls and backpressure.
struct TcpConnection::FileOwner
{
    int fd = -1;
    ~FileOwner() { if (fd >= 0) ::close(fd); }
};

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null!\n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

TcpConnection::TcpConnection(EventLoop *loop,
                             const std::string &nameArg,
                             int sockfd,
                             const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(CheckLoopNotNull(loop))
    , name_(nameArg)
    , state_(kConnecting)
    , reading_(true)
    , socket_(makePoolUnique<Socket>(sockfd))
    , channel_(makePoolUnique<Channel>(loop, sockfd))
    , localAddr_(localAddr)
    , peerAddr_(peerAddr)
    , highWaterMark_(64 * 1024 * 1024) // 64M
    , sendFileFd_(-1)
    , sendFileOffset_(0)
    , sendFileRemaining_(0)
    , writeCompleteScheduled_(false)
{
    // 下面给channel设置相应的回调函数 poller给channel通知感兴趣的事件发生了 channel会回调相应的回调函数
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this));

    LOG_DEBUG("TcpConnection::ctor[%s] at fd=%d\n", name_.c_str(), sockfd);
    socket_->setKeepAlive(true);
    socket_->setTcpNoDelay(true);
    // Preserve Linux TCP autotuning. Setting SO_RCVBUF locks the receive window
    // at a value that may be clamped by rmem_max, preventing later growth.
}

bool TcpConnection::isWriting() const
{
    return channel_ && channel_->isWriting();
}

TcpConnection::~TcpConnection()
{
    LOG_DEBUG("TcpConnection::dtor[%s] at fd=%d state=%d\n", name_.c_str(), channel_->fd(), (int)state_);
}

void TcpConnection::send(const std::string &buf)
{
    send(buf.data(), buf.size());
}

void TcpConnection::send(const void *data, size_t len)
{
    if (state_ != kConnected || data == nullptr || len == 0)
    {
        return;
    }

    if (loop_->isInLoopThread())
    {
        sendInLoop(data, len);
        return;
    }

    // 拷贝进内存池上的 string。bind 只复制 shared_ptr，不再把调用方的指针带进别的线程。
    std::shared_ptr<PoolString> message = makePoolShared<PoolString>(
        static_cast<const char *>(data), len);
    loop_->runInLoop(std::bind(&TcpConnection::sendOwnedInLoop, shared_from_this(), message));
}

void TcpConnection::sendOwnedInLoop(const std::shared_ptr<PoolString> &message)
{
    sendInLoop(message->data(), message->size());
}

/**
 * 发送数据 应用写的快 而内核发送数据慢 需要把待发送数据写入缓冲区，而且设置了水位回调
 **/
void TcpConnection::sendInLoop(const void *data, size_t len)
{
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    if (state_ == kDisconnected) // 之前调用过该connection的shutdown 不能再进行发送了
    {
        LOG_ERROR("disconnected, give up writing");
        return;
    }

    // 表示channel_第一次开始写数据或者缓冲区没有待发送数据
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::send(channel_->fd(), data, len, MSG_NOSIGNAL);
        if (nwrote >= 0)
        {
            remaining = len - nwrote;
            if (remaining == 0)
            {
                // 合并 writeComplete，避免推流时每次 send 都塞进 pending 队列。
                scheduleWriteComplete();
            }
        }
        else // nwrote < 0
        {
            nwrote = 0;
            if (errno != EWOULDBLOCK && errno != EAGAIN)
            {
                LOG_ERROR("TcpConnection::sendInLoop");
                if (errno == EPIPE || errno == ECONNRESET) // SIGPIPE RESET
                {
                    faultError = true;
                }
            }
        }
    }
    /**
     * 说明当前这一次write并没有把数据全部发送出去 剩余的数据需要保存到缓冲区当中
     * 然后给channel注册EPOLLOUT事件，Poller发现tcp的发送缓冲区有空间后会通知
     * 相应的sock->channel，调用channel对应注册的writeCallback_回调方法，
     * channel的writeCallback_实际上就是TcpConnection设置的handleWrite回调，
     * 把发送缓冲区outputBuffer_的内容全部发送完成
     **/
    if (!faultError && remaining > 0)
    {
        // 目前发送缓冲区剩余的待发送的数据的长度
        size_t oldLen = outputBuffer_.readableBytes();
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ && highWaterMarkCallback_)
        {
            loop_->queueInLoop(
                std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }
        outputBuffer_.append(static_cast<const char *>(data) + nwrote, remaining);
        if (!channel_->isWriting())
        {
            channel_->enableWriting(); // 这里一定要注册channel的写事件 否则poller不会给channel通知epollout
        }
    }
}

void TcpConnection::scheduleWriteComplete()
{
    if (!writeCompleteCallback_ || writeCompleteScheduled_)
    {
        return;
    }
    writeCompleteScheduled_ = true;
    loop_->queueInLoop(std::bind(&TcpConnection::invokeWriteComplete, shared_from_this()));
}

void TcpConnection::invokeWriteComplete()
{
    writeCompleteScheduled_ = false;
    // 输出缓冲空闲时才通知上层继续灌数，避免回调风暴打爆 EventLoop。
    if (writeCompleteCallback_ && outputBuffer_.readableBytes() == 0 && !channel_->isWriting())
    {
        writeCompleteCallback_(shared_from_this());
    }
}

void TcpConnection::shutdown()
{
    if (state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->runInLoop(
            std::bind(&TcpConnection::shutdownInLoop, shared_from_this()));
    }
}

void TcpConnection::shutdownInLoop()
{
    if (!channel_->isWriting()) // 说明当前outputBuffer_的数据全部向外发送完成
    {
        socket_->shutdownWrite();
    }
}

void TcpConnection::forceClose()
{
    if (state_ == kConnected || state_ == kDisconnecting)
    {
        setState(kDisconnecting);
        loop_->queueInLoop(std::bind(&TcpConnection::forceCloseInLoop, shared_from_this()));
    }
}

void TcpConnection::forceCloseInLoop()
{
    if (state_ == kConnected || state_ == kDisconnecting)
    {
        handleClose();
    }
}

// 连接建立
void TcpConnection::connectEstablished()
{
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading(); // 向poller注册channel的EPOLLIN读事件

    // 新连接建立 执行回调
    if (connectionCallback_)
    {
        connectionCallback_(shared_from_this());
    }
}
// 连接销毁
void TcpConnection::connectDestroyed()
{
    sendFileRemaining_ = 0;
    sendFileOwner_.reset();
    if (state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll(); // 把channel的所有感兴趣的事件从poller中删除掉
        if (connectionCallback_)
        {
            connectionCallback_(shared_from_this());
        }
    }
    channel_->remove(); // 把channel从poller中删除掉
}

// 读是相对服务器而言的 当对端客户端有数据到达 服务器端检测到EPOLLIN 就会触发该fd上的回调 handleRead取读走对端发来的数据
void TcpConnection::handleRead(Timestamp receiveTime)
{
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0) // 有数据到达
    {
        // 已建立连接的用户有可读事件发生了 调用用户传入的回调操作onMessage shared_from_this就是获取了TcpConnection的智能指针
        if (messageCallback_)
        {
            messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
        }
    }
    else if (n == 0) // 客户端断开
    {
        handleClose();
    }
    else // 出错了
    {
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead");
        handleError();
    }
}

void TcpConnection::handleWrite()
{
    if (!channel_->isWriting())
    {
        LOG_ERROR("TcpConnection fd=%d is down, no more writing", channel_->fd());
        return;
    }

    // 没有待发的用户数据时，继续上次没写完的 sendfile。
    if (outputBuffer_.readableBytes() == 0)
    {
        if (sendFileRemaining_ > 0)
        {
            sendFileInLoop(sendFileFd_, sendFileOffset_, sendFileRemaining_);
        }
        else
        {
            channel_->disableWriting();
            scheduleWriteComplete();
            if (state_ == kDisconnecting)
            {
                shutdownInLoop();
            }
        }
        return;
    }

    int savedErrno = 0;
    // writeFd 内部会写到 EAGAIN，并直接推进 readerIndex_。
    const ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
    if (n >= 0)
    {
        if (outputBuffer_.readableBytes() == 0)
        {
            if (sendFileRemaining_ > 0)
            {
                sendFileInLoop(sendFileFd_, sendFileOffset_, sendFileRemaining_);
                return;
            }
            channel_->disableWriting();
            scheduleWriteComplete();
            if (state_ == kDisconnecting)
            {
                shutdownInLoop(); // 在当前所属的loop中把TcpConnection删除掉
            }
        }
    }
    else
    {
        LOG_ERROR("TcpConnection::handleWrite");
    }
}

void TcpConnection::handleClose()
{
    sendFileRemaining_ = 0;
    sendFileOwner_.reset();
    LOG_DEBUG("TcpConnection::handleClose fd=%d state=%d\n", channel_->fd(), (int)state_);
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr connPtr(shared_from_this());
    if (connectionCallback_)
    {
        connectionCallback_(connPtr); // 连接回调
    }
    if (closeCallback_)
    {
        closeCallback_(connPtr); // 执行关闭连接的回调 执行的是TcpServer::removeConnection回调方法
    }
}

void TcpConnection::handleError()
{
    int optval;
    socklen_t optlen = sizeof optval;
    int err = 0;
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err = errno;
    }
    else
    {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError name:%s - SO_ERROR:%d\n", name_.c_str(), err);
}

// 新增的零拷贝发送函数
void TcpConnection::sendFile(int fileDescriptor, off_t offset, size_t count) {
    if (connected()) {
        // Keep a duplicate alive across threads and EPOLLOUT waits, even if the
        // caller closes its descriptor after an early protocol error.
        // Allocate before dup: allocation failure cannot leak a descriptor.
        auto owner = makePoolShared<FileOwner>();
        const int owned = ::fcntl(fileDescriptor, F_DUPFD_CLOEXEC, 0);
        if (owned < 0)
        {
            LOG_ERROR("TcpConnection::sendFile dup failed errno=%d\n", errno);
            forceClose();
            return;
        }
        owner->fd = owned;
        loop_->runInLoop([self = shared_from_this(), owner, offset, count] {
            if (self->sendFileRemaining_ != 0)
            {
                LOG_ERROR("TcpConnection::sendFile overlapping file sends\n");
                self->forceClose();
                return;
            }
            self->sendFileOwner_ = owner;
            self->sendFileInLoop(owner->fd, offset, count);
        });
    } else {
        LOG_ERROR("TcpConnection::sendFile - not connected");
    }
}

// 在事件循环中执行 sendfile。内核缓冲满了就等 EPOLLOUT，不再把自身塞回待执行队列空转。
void TcpConnection::sendFileInLoop(int fileDescriptor, off_t offset, size_t count)
{
    if (state_ == kDisconnecting || state_ == kDisconnected)
    {
        LOG_ERROR("disconnected, give up writing");
        sendFileRemaining_ = 0;
        sendFileOwner_.reset();
        if (outputBuffer_.readableBytes() == 0 && channel_->isWriting())
        {
            channel_->disableWriting();
            if (state_ == kDisconnecting)
            {
                shutdownInLoop();
            }
        }
        return;
    }

    if (count == 0)
    {
        sendFileRemaining_ = 0;
        sendFileOwner_.reset();
        if (outputBuffer_.readableBytes() == 0 && channel_->isWriting())
        {
            channel_->disableWriting();
        }
        return;
    }

    // 输出缓冲还有数据时先排队，等 handleWrite 把缓冲发完再继续发文件。
    if (outputBuffer_.readableBytes() > 0)
    {
        sendFileFd_ = fileDescriptor;
        sendFileOffset_ = offset;
        sendFileRemaining_ = count;
        if (!channel_->isWriting())
        {
            channel_->enableWriting();
        }
        return;
    }

    ssize_t bytesSent;
    do
    {
        bytesSent = ::sendfile(socket_->fd(), fileDescriptor, &offset,
                               std::min(count, size_t(1024 * 1024)));
    } while (bytesSent < 0 && errno == EINTR);
    if (bytesSent > 0)
    {
        lastFileWriteTime_ = std::chrono::steady_clock::now();
        if (static_cast<size_t>(bytesSent) >= count)
        {
            count = 0;
        }
        else
        {
            count -= static_cast<size_t>(bytesSent);
        }
        if (count == 0)
        {
            sendFileRemaining_ = 0;
            sendFileOwner_.reset();
            if (channel_->isWriting())
            {
                channel_->disableWriting();
            }
            scheduleWriteComplete();
            if (state_ == kDisconnecting)
            {
                shutdownInLoop();
            }
            return;
        }
    }
    else if (bytesSent == 0)
    {
        sendFileRemaining_ = 0;
        sendFileOwner_.reset();
        if (channel_->isWriting())
        {
            channel_->disableWriting();
        }
        LOG_ERROR("TcpConnection::sendFileInLoop unexpected EOF\n");
        forceClose();
        return;
    }
    else if (errno != EWOULDBLOCK && errno != EAGAIN)
    {
        LOG_ERROR("TcpConnection::sendFileInLoop");
        sendFileRemaining_ = 0;
        sendFileOwner_.reset();
        if (channel_->isWriting())
        {
            channel_->disableWriting();
        }
        forceClose();
        return;
    }

    sendFileFd_ = fileDescriptor;
    sendFileOffset_ = offset;
    sendFileRemaining_ = count;
    if (!channel_->isWriting())
    {
        channel_->enableWriting();
    }
}
