#include "Connector.h"

#include <algorithm>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "Channel.h"
#include "EventLoop.h"
#include "Logger.h"

namespace
{

int createNonblocking()
{
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0)
    {
        LOG_FATAL("%s:%s:%d socket create err:%d\n", __FILE__, __FUNCTION__, __LINE__, errno);
    }
    return sockfd;
}

} // namespace

const int Connector::kMaxRetryDelayMs = 30 * 1000;
const int Connector::kInitRetryDelayMs = 500;

Connector::Connector(EventLoop *loop, const InetAddress &serverAddr)
    : loop_(loop)
    , serverAddr_(serverAddr)
    , connect_(false)
    , state_(kDisconnected)
    , retryDelayMs_(kInitRetryDelayMs)
{
}

Connector::~Connector()
{
    loop_->cancel(timerId_);
    if (channel_)
    {
        channel_->disableAll();
        channel_->remove();
    }
}

void Connector::start()
{
    connect_ = true;
    loop_->runInLoop(std::bind(&Connector::startInLoop, shared_from_this()));
}

void Connector::startInLoop()
{
    if (connect_ && state_ == kDisconnected)
    {
        connect();
    }
}

void Connector::stop()
{
    connect_ = false;
    loop_->queueInLoop(std::bind(&Connector::stopInLoop, shared_from_this()));
}

void Connector::stopInLoop()
{
    loop_->cancel(timerId_);
    if (state_ == kConnecting)
    {
        setState(kDisconnected);
        const int sockfd = removeAndResetChannel();
        if (sockfd >= 0)
        {
            ::close(sockfd);
        }
    }
}

void Connector::restart()
{
    setState(kDisconnected);
    retryDelayMs_ = kInitRetryDelayMs;
    connect_ = true;
    startInLoop();
}

void Connector::connect()
{
    const int sockfd = createNonblocking();
    const int ret = ::connect(sockfd,
                              reinterpret_cast<const sockaddr *>(serverAddr_.getSockAddr()),
                              sizeof(sockaddr_in));
    const int savedErrno = (ret == 0) ? 0 : errno;
    switch (savedErrno)
    {
    case 0:
    case EINPROGRESS:
    case EINTR:
    case EISCONN:
        connecting(sockfd);
        break;
    case EAGAIN:
    case EADDRINUSE:
    case EADDRNOTAVAIL:
    case ECONNREFUSED:
    case ENETUNREACH:
        retry(sockfd);
        break;
    default:
        LOG_ERROR("Connector::connect unexpected error=%d\n", savedErrno);
        ::close(sockfd);
        break;
    }
}

void Connector::connecting(int sockfd)
{
    setState(kConnecting);
    channel_.reset(new Channel(loop_, sockfd));
    channel_->setWriteCallback(std::bind(&Connector::handleWrite, this));
    channel_->setErrorCallback(std::bind(&Connector::handleError, this));
    channel_->enableWriting();
}

int Connector::removeAndResetChannel()
{
    if (!channel_)
    {
        return -1;
    }
    channel_->disableAll();
    channel_->remove();
    const int sockfd = channel_->fd();
    loop_->queueInLoop(std::bind(&Connector::resetChannel, shared_from_this()));
    return sockfd;
}

void Connector::resetChannel()
{
    channel_.reset();
}

void Connector::handleWrite()
{
    if (state_ != kConnecting)
    {
        return;
    }

    const int sockfd = removeAndResetChannel();
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
    {
        err = errno;
    }

    if (err)
    {
        LOG_ERROR("Connector::handleWrite SO_ERROR=%d to %s\n",
                  err,
                  serverAddr_.toIpPort().c_str());
        retry(sockfd);
        return;
    }

    setState(kConnected);
    retryDelayMs_ = kInitRetryDelayMs;
    if (connect_ && newConnectionCallback_)
    {
        newConnectionCallback_(sockfd);
    }
    else
    {
        ::close(sockfd);
    }
}

void Connector::handleError()
{
    if (state_ == kConnecting)
    {
        const int sockfd = removeAndResetChannel();
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len);
        LOG_ERROR("Connector::handleError SO_ERROR=%d to %s\n",
                  err,
                  serverAddr_.toIpPort().c_str());
        retry(sockfd);
    }
}

void Connector::retry(int sockfd)
{
    ::close(sockfd);
    setState(kDisconnected);
    if (!connect_)
    {
        return;
    }

    LOG_INFO("Connector::retry connecting to %s in %d ms\n",
             serverAddr_.toIpPort().c_str(),
             retryDelayMs_);
    timerId_ = loop_->runAfter(retryDelayMs_ / 1000.0,
                               std::bind(&Connector::startInLoop, shared_from_this()));
    retryDelayMs_ = std::min(retryDelayMs_ * 2, kMaxRetryDelayMs);
}
