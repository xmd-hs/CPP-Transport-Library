#pragma once

#include <cstdint>

class Timer;

// 定时器句柄，用于 cancel。
class TimerId
{
public:
    TimerId() : timer_(nullptr), sequence_(0) {}
    TimerId(Timer *timer, int64_t seq) : timer_(timer), sequence_(seq) {}

    Timer *timer() const { return timer_; }
    int64_t sequence() const { return sequence_; }

private:
    friend class TimerQueue;
    Timer *timer_;
    int64_t sequence_;
};
