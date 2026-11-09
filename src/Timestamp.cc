#include <stdio.h>
#include <time.h>

#include "Timestamp.h"

Timestamp Timestamp::now()
{
    struct timespec ts;
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0)
    {
        return Timestamp(static_cast<int64_t>(::time(nullptr)) * kMicroSecondsPerSecond);
    }
    return Timestamp(static_cast<int64_t>(ts.tv_sec) * kMicroSecondsPerSecond +
                     ts.tv_nsec / 1000);
}

std::string Timestamp::toString() const
{
    char buf[64];
    const time_t seconds = secondsSinceEpoch();
    struct tm tm_time;
    localtime_r(&seconds, &tm_time);
    snprintf(buf, sizeof(buf), "%4d/%02d/%02d %02d:%02d:%02d",
             tm_time.tm_year + 1900,
             tm_time.tm_mon + 1,
             tm_time.tm_mday,
             tm_time.tm_hour,
             tm_time.tm_min,
             tm_time.tm_sec);
    return buf;
}
