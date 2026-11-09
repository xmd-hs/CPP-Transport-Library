#include <memory>

#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"
#include "Logger.h"
EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg)
    : baseLoop_(baseLoop), name_(nameArg), started_(false), numThreads_(0), next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool()
{
    // Don't delete loop, it's stack variable
}

void EventLoopThreadPool::start(const ThreadInitCallback &cb)
{
    started_ = true;

    for (int i = 0; i < numThreads_; ++i)
    {
        std::string threadName = name_ + std::to_string(i);
        PoolUniquePtr<EventLoopThread> thread = makePoolUnique<EventLoopThread>(cb, threadName);
        loops_.push_back(thread->startLoop()); // 底层创建线程 绑定一个新的EventLoop 并返回该loop的地址
        threads_.push_back(std::move(thread));
    }

    if (numThreads_ == 0 && cb) // 整个服务端只有一个线程运行baseLoop
    {
        cb(baseLoop_);
    }
}

// 如果工作在多线程中，baseLoop_(mainLoop)会默认以轮询的方式分配Channel给subLoop
EventLoop *EventLoopThreadPool::getNextLoop()
{
    // 如果只设置一个线程 也就是只有一个mainReactor 无subReactor
    // 那么轮询只有一个线程 getNextLoop()每次都返回当前的baseLoop_
    EventLoop *loop = baseLoop_;

    // 通过轮询获取下一个处理事件的loop
    // 如果没设置多线程数量，则不会进去，相当于直接返回baseLoop
    if(!loops_.empty())
    {
        loop = loops_[next_];
        ++next_;
        // 轮询
        if(next_ >= loops_.size())
        {
            next_ = 0;
        }
    }

    return loop;
}



PoolVector<EventLoop *> EventLoopThreadPool::getAllLoops()
{
    if (loops_.empty())
    {
        return PoolVector<EventLoop *>(1, baseLoop_);
    }
    else
    {
        return loops_;
    }
}