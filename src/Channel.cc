#include <sys/epoll.h>

#include "Channel.h"
#include "EventLoop.h"
#include "Logger.h"

// 与 epoll 事件位对应；读写拆开是为了 enable/disable 时按位或/与
const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI; // 普通数据 + 带外数据
const int Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop)
    , fd_(fd)
    , events_(0)
    , revents_(0)
    , index_(-1)   // -1 == Poller 侧的 kNew：尚未加入 epoll
    , tied_(false)
{
}

Channel::~Channel()
{
}

/**
 * 生命周期防护：TcpConnection 以 shared_ptr 存在，其 Channel 回调绑的是成员函数。
 * 若连接已析构而 Channel 仍被 Poller 调到，就会悬空。
 * tie 保存 weak_ptr；handleEvent 里 lock 成功才继续，失败说明对象已没了，直接跳过。
 *
 * 调用点：TcpConnection 构造完成后 channel_->tie(shared_from_this())。
 */
void Channel::tie(const std::shared_ptr<void> &obj)
{
    tie_ = obj;
    tied_ = true;
}

/**
 * events_ 变了之后，必须通知 Poller 做 epoll_ctl(ADD/MOD/DEL)。
 * Channel 不直接碰 epoll，统一交给所属 EventLoop，保证线程归属清晰。
 */
void Channel::update()
{
    loop_->updateChannel(this);
}

/** 从 EventLoop / Poller 中移除对本 fd 的监听（连接关闭时） */
void Channel::remove()
{
    loop_->removeChannel(this);
}

void Channel::handleEvent(Timestamp receiveTime)
{
    if (tied_)
    {
        // 提升失败 → TcpConnection 已销毁，不能再调它的成员回调
        std::shared_ptr<void> guard = tie_.lock();
        if (guard)
        {
            handleEventWithGuard(receiveTime);
        }
    }
    else
    {
        // 未 tie：如 Acceptor、wakeup Channel，生命周期由 EventLoop 直接管
        handleEventWithGuard(receiveTime);
    }
}

/**
 * 按 revents_ 分发。顺序有意设计：
 * 1) EPOLLHUP 且无 EPOLLIN → 对端关闭 / 本端 shutdown 写端后的挂起，走 close
 * 2) EPOLLERR              → 错误回调
 * 3) EPOLLIN | EPOLLPRI    → 可读
 * 4) EPOLLOUT              → 可写（发送缓冲从满变为可写时）
 *
 * 注意：一次返回里多个位可能同时置位，所以用多个独立 if，不是 if-else。
 */
void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    LOG_DEBUG("channel handleEvent revents:%d\n", revents_);

    // 对端关闭连接，或本端 shutdownWrite 后内核回报 HUP；此时通常没有可读数据
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN))
    {
        if (closeCallback_)
        {
            closeCallback_();
        }
    }

    if (revents_ & EPOLLERR)
    {
        if (errorCallback_)
        {
            errorCallback_();
        }
    }

    if (revents_ & (EPOLLIN | EPOLLPRI))
    {
        if (readCallback_)
        {
            readCallback_(receiveTime);
        }
    }

    if (revents_ & EPOLLOUT)
    {
        if (writeCallback_)
        {
            writeCallback_();
        }
    }
}
