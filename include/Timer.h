#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include "Timestamp.h"
#include "noncopyable.h"

class Timer : noncopyable
{
public:
    using TimerCallback = std::function<void()>;

    Timer(TimerCallback cb, Timestamp when, double interval)
        : callback_(std::move(cb))
        , expiration_(when)
        , interval_(interval)
        , repeat_(interval > 0.0)
        , sequence_(s_numCreated_.fetch_add(1) + 1)
    {
    }

    void run() const { callback_(); }

    Timestamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    int64_t sequence() const { return sequence_; }

    void restart(Timestamp now)
    {
        if (repeat_)
        {
            expiration_ = now.addSeconds(interval_);
        }
        else
        {
            expiration_ = Timestamp::invalid();
        }
    }

private:
    const TimerCallback callback_;
    Timestamp expiration_;
    const double interval_;
    const bool repeat_;
    const int64_t sequence_;

    static std::atomic_int64_t s_numCreated_;
};
