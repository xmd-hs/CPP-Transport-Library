#include <chrono>
#include <cstdio>
#include <functional>
#include "EventLoop.h"

int main()
{
    EventLoop loop;
    constexpr int rounds = 20000;
    constexpr int batchSize = 32;
    int batches = 0;
    int calls = 0;
    std::function<void()> next;
    next = [&] {
        for (int i = 0; i < batchSize; ++i)
            loop.queueInLoop([&] { ++calls; });
        loop.queueInLoop([&] {
            if (++batches < rounds) next();
            else loop.quit();
        });
    };
    next();
    const auto start = std::chrono::steady_clock::now();
    loop.queueInLoop([] {}); // ensure the first pending batch wakes the loop
    loop.wakeup();
    loop.loop();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("eventloop callbacks=%d seconds=%.6f Mcallbacks_s=%.3f\n",
                calls, seconds, calls / seconds / 1000000);
    return calls == rounds * batchSize ? 0 : 1;
}
