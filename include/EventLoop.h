#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "CurrentThread.h"
#include "KamaMemory.h"
#include "Timestamp.h"
#include "noncopyable.h"

class Channel;
class Poller;
class TimerId;
class TimerQueue;

class EventLoop : noncopyable
{
public:
    using Functor = std::function<void()>;
    using TimerCallback = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    Timestamp pollReturnTime() const { return pollRetureTime_; }

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);

    TimerId runAt(Timestamp time, TimerCallback cb);
    TimerId runAfter(double delaySeconds, TimerCallback cb);
    TimerId runEvery(double intervalSeconds, TimerCallback cb);
    void cancel(TimerId timerId);

    void wakeup();

    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);
    bool hasChannel(Channel *channel);

    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }

private:
    void handleRead();
    void doPendingFunctors();

    using ChannelList = PoolVector<Channel *>;

    std::atomic_bool looping_;
    std::atomic_bool quit_;
    const pid_t threadId_;
    Timestamp pollRetureTime_;
    PoolUniquePtr<Poller> poller_;

    int wakeupFd_;
    std::atomic_bool wakeupPending_;
    PoolUniquePtr<Channel> wakeupChannel_;
    PoolUniquePtr<TimerQueue> timerQueue_;

    ChannelList activeChannels_;

    std::atomic_bool callingPendingFunctors_;
    PoolVector<Functor> pendingFunctors_;
    PoolVector<Functor> runningFunctors_; // retain both batch allocations on the loop thread
    std::mutex mutex_;
};
