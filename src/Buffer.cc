#include <errno.h>
#include <algorithm>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "Buffer.h"

/**
 * 从fd上读取数据 Poller工作在LT模式
 * Buffer缓冲区是有大小的！ 但是从fd上读取数据的时候 却不知道tcp数据的最终大小
 *
 * @description: 从socket读到缓冲区的方法是使用readv先读至buffer_，
 * Buffer_空间如果不够会读入到栈上65536个字节大小的空间，然后以append的
 * 方式追加入buffer_。既考虑了避免系统调用带来开销，又不影响数据的接收。
 **/
ssize_t Buffer::readFd(int fd, int *saveErrno)
{
    // 线程内复用，避免每次读都在栈上新碰 64KB。不初始化，readv 会覆盖读到的字节。
    thread_local char extrabuf[65536];
    ssize_t total = 0;
    constexpr size_t kReadBudget = 256 * 1024;

    // 内核里还有数据时继续读，最多 8 次。读不满说明这一轮已经取完。
    // 仍然只在 TcpConnection::handleRead 里回调一次 onMessage。
    for (int i = 0; i < 8; ++i)
    {
        const size_t budget = kReadBudget - static_cast<size_t>(total);
        const size_t writable = std::min(writableBytes(), budget);
        struct iovec vec[2];
        vec[0].iov_base = begin() + writerIndex_;
        vec[0].iov_len = writable;
        vec[1].iov_base = extrabuf;
        vec[1].iov_len = std::min(sizeof(extrabuf), budget - writable);

        // 可写空间不足 64KB 时用第二块暂存，避免为了一次读去扩容。
        const int iovcnt = vec[1].iov_len > 0 ? 2 : 1;
        const size_t requested = writable + (iovcnt == 2 ? vec[1].iov_len : 0);
        const ssize_t n = ::readv(fd, vec, iovcnt);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                --i;
                continue;
            }
            if (total > 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                return total;
            }
            *saveErrno = errno;
            return -1;
        }
        if (n == 0)
        {
            if (total > 0)
            {
                return total;
            }
            return 0;
        }
        if (static_cast<size_t>(n) <= writable)
        {
            writerIndex_ += static_cast<size_t>(n);
        }
        else
        {
            writerIndex_ = buffer_.size();
            append(extrabuf, static_cast<size_t>(n) - writable);
        }
        total += n;
        if (static_cast<size_t>(n) < requested || static_cast<size_t>(total) >= kReadBudget)
        {
            break;
        }
    }
    return total;
}

// inputBuffer_.readFd表示将对端数据读到inputBuffer_中，移动writerIndex_指针
// outputBuffer_.writeFd标示将数据写入到outputBuffer_中，从readerIndex_开始，可以写readableBytes()个字节
ssize_t Buffer::writeFd(int fd, int *saveErrno)
{
    ssize_t total = 0;
    while (readableBytes() > 0)
    {
        const ssize_t n = ::send(fd, peek(), readableBytes(), MSG_NOSIGNAL);
        if (n > 0)
        {
            retrieve(static_cast<size_t>(n));
            total += n;
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
                return total; // 内核缓冲满了，等下一次 EPOLLOUT
            }
            *saveErrno = errno;
            return total > 0 ? total : -1;
        }
        break;
    }
    return total;
}
