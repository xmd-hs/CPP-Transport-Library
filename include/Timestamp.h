#pragma once

#include <cstdint>
#include <string>

class Timestamp
{
public:
    static const int kMicroSecondsPerSecond = 1000 * 1000;

    Timestamp() : microSecondsSinceEpoch_(0) {}
    explicit Timestamp(int64_t microSecondsSinceEpoch)
        : microSecondsSinceEpoch_(microSecondsSinceEpoch)
    {
    }

    static Timestamp now();
    static Timestamp invalid() { return Timestamp(); }

    std::string toString() const;

    int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }
    time_t secondsSinceEpoch() const
    {
        return static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond);
    }

    bool valid() const { return microSecondsSinceEpoch_ > 0; }

    Timestamp addSeconds(double seconds) const
    {
        const int64_t delta =
            static_cast<int64_t>(seconds * kMicroSecondsPerSecond);
        return Timestamp(microSecondsSinceEpoch_ + delta);
    }

private:
    int64_t microSecondsSinceEpoch_;
};

inline bool operator<(Timestamp lhs, Timestamp rhs)
{
    return lhs.microSecondsSinceEpoch() < rhs.microSecondsSinceEpoch();
}

inline bool operator==(Timestamp lhs, Timestamp rhs)
{
    return lhs.microSecondsSinceEpoch() == rhs.microSecondsSinceEpoch();
}

inline double timeDifference(Timestamp high, Timestamp low)
{
    const int64_t diff = high.microSecondsSinceEpoch() - low.microSecondsSinceEpoch();
    return static_cast<double>(diff) / Timestamp::kMicroSecondsPerSecond;
}
