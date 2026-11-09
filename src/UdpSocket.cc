#include "UdpSocket.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <memory>
#include <vector>

#include "Channel.h"
#include "EventLoop.h"
#include "Logger.h"

namespace
{

int createUdpNonblocking()
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        LOG_FATAL("UdpSocket socket create err:%d\n", errno);
    }
    return fd;
}

} // namespace

UdpSocket::UdpSocket(EventLoop *loop, const InetAddress &localAddr)
    : loop_(loop)
    , sockfd_(createUdpNonblocking())
    , localAddr_(localAddr)
    , started_(false)
{
    int on = 1;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (::bind(sockfd_,
               reinterpret_cast<const sockaddr *>(localAddr_.getSockAddr()),
               sizeof(sockaddr_in)) < 0)
    {
        LOG_FATAL("UdpSocket bind %s failed errno=%d\n",
                  localAddr_.toIpPort().c_str(),
                  errno);
    }

    sockaddr_in actual;
    socklen_t len = sizeof(actual);
    if (::getsockname(sockfd_, reinterpret_cast<sockaddr *>(&actual), &len) == 0)
    {
        localAddr_.setSockAddr(actual);
    }

    channel_.reset(new Channel(loop_, sockfd_));
    // ReadEventCallback 带 Timestamp，显式吞掉参数，避免 bind 可调用性差异。
    channel_->setReadCallback([this](Timestamp) { handleRead(); });
}

UdpSocket::~UdpSocket()
{
    stop();
    ::close(sockfd_);
}

void UdpSocket::start()
{
    if (started_)
    {
        return;
    }
    started_ = true;
    loop_->runInLoop([this]() { channel_->enableReading(); });
}

void UdpSocket::stop()
{
    if (!started_)
    {
        return;
    }
    started_ = false;
    loop_->runInLoop([this]() {
        channel_->disableAll();
        channel_->remove();
    });
}

ssize_t UdpSocket::sendTo(const void *data, size_t len, const InetAddress &peer)
{
    return ::sendto(sockfd_,
                    data,
                    len,
                    0,
                    reinterpret_cast<const sockaddr *>(peer.getSockAddr()),
                    sizeof(sockaddr_in));
}

void UdpSocket::handleRead()
{
    thread_local std::vector<char> buf(64 * 1024);
    while (true)
    {
        sockaddr_in peerAddr;
        socklen_t addrLen = sizeof(peerAddr);
        const ssize_t n = ::recvfrom(sockfd_,
                                     buf.data(),
                                     buf.size(),
                                     0,
                                     reinterpret_cast<sockaddr *>(&peerAddr),
                                     &addrLen);
        if (n > 0)
        {
            if (messageCallback_)
            {
                messageCallback_(buf.data(), static_cast<size_t>(n), InetAddress(peerAddr));
            }
            continue;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            LOG_ERROR("UdpSocket::handleRead errno=%d\n", errno);
        }
        break;
    }
}
