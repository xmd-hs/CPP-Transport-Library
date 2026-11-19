#pragma once

#include <functional>
#include <memory>

#include "noncopyable.h"
#include "Timestamp.h"

class EventLoop;

/**
 * Channel —— Reactor 模型里的「事件分发器」单元
 *
 * 在整条调用链里，三者职责要分清：
 *   EventLoop  ：拥有一个 Poller，在当前线程里不断 loop
 *   Poller     ：封装 epoll_ctl / epoll_wait，负责「监听」
 *   Channel    ：封装「一个 fd + 感兴趣的事件 + 回调」，负责「分发」
 *
 * 一个 Channel 只属于一个 EventLoop（one loop per thread），
 * 也只管理一个 fd。业务对象（如 TcpConnection、Acceptor）各自持有自己的 Channel，
 * 把成员函数绑到 read/write/close/error 回调上。
 *
 * 典型数据流：
 *   enableReading() 修改 events_
 *     → update() → EventLoop::updateChannel → Poller::updateChannel → epoll_ctl
 *   epoll_wait 返回后 Poller 调用 set_revents(实际发生的事件)
 *     → EventLoop::loop 调用 Channel::handleEvent
 *     → 按 revents_ 分发给对应回调（通常落到 TcpConnection::handleRead 等）
 **/
class Channel : noncopyable
{
public:
    using EventCallback = std::function<void()>;
    // 读回调多带一个时间戳：epoll_wait 返回时刻，便于日志 / 空闲检测
    using ReadEventCallback = std::function<void(Timestamp)>;

    /**
     * @param loop 所属的 EventLoop，后续 update/remove 都转发到它
     * @param fd   被监听的描述符（socket、eventfd、timerfd 等）
     */
    Channel(EventLoop *loop, int fd);
    ~Channel();

    /**
     * fd 在 Poller 里就绪后，由 EventLoop::loop() 调用。
     * 若已 tie 过 TcpConnection，会先用 weak_ptr::lock 保证对象还活着，
     * 再进入 handleEventWithGuard 按 revents_ 分发回调。
     */
    void handleEvent(Timestamp receiveTime);

    // ---------- 设置回调（通常在 TcpConnection / Acceptor 构造时绑一次）----------
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    /**
     * 把 Channel 与某个 shared_ptr 管理的对象「绑」在一起（典型是 TcpConnection）。
     * 回调执行前 lock 一下：若连接已销毁则跳过，避免悬空 this。
     * Acceptor 等非 shared_ptr 管理的对象不必调用。
     */
    void tie(const std::shared_ptr<void> &);

    int fd() const { return fd_; }
    int events() const { return events_; }           // 用户关心的事件（写给 epoll）
    void set_revents(int revt) { revents_ = revt; }  // Poller 填入「实际发生」的事件

    /**
     * 修改感兴趣事件，并立刻通过 update() 同步到 epoll。
     * 注意：这里改的是用户态 events_；真正生效靠 loop_->updateChannel → epoll_ctl。
     */
    void enableReading() { events_ |= kReadEvent; update(); }
    void disableReading() { events_ &= ~kReadEvent; update(); }
    void enableWriting() { events_ |= kWriteEvent; update(); }
    void disableWriting() { events_ &= ~kWriteEvent; update(); }
    void disableAll() { events_ = kNoneEvent; update(); }

    bool isNoneEvent() const { return events_ == kNoneEvent; }
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; }

    /**
     * index_ 给 EPollPoller 用，标记该 Channel 在 epoll 中的状态：
     *   -1  kNew     从未添加
     *    1  kAdded   已在 epoll 中
     *    2  kDeleted 曾添加后又从关注列表摘掉（fd 仍可能开着）
     * Channel 本身不解释这些魔数，只负责存取。
     */
    int index() { return index_; }
    void set_index(int idx) { index_ = idx; }

    EventLoop *ownerLoop() { return loop_; }

    /** 从所属 EventLoop / Poller 中注销自己（通常在连接关闭路径上调用） */
    void remove();

private:
    /** 把当前 events_ 同步到 Poller（内部走 loop_->updateChannel） */
    void update();
    /** 已确认对象存活（或未 tie）后，按 revents_ 调用各回调 */
    void handleEventWithGuard(Timestamp receiveTime);

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop *loop_; // 所属事件循环；保证 Channel 只在该线程被操作
    const int fd_;    // 被监听的 fd，构造后不变
    int events_;      // 感兴趣的事件掩码（EPOLLIN / EPOLLOUT ...）
    int revents_;     // 本次 poll 实际发生的事件掩码（由 Poller 写入）
    int index_;       // 在 Poller 中的状态标记，见上方注释

    std::weak_ptr<void> tie_; // 绑定的上层对象（弱引用，不延长寿命）
    bool tied_;               // 是否调用过 tie()

    // 四个回调：谁注册、谁负责业务。Channel 只做「事件 → 回调」的分发。
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
