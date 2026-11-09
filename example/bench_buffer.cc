#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "Buffer.h"

using Clock = std::chrono::steady_clock;

int main()
{
    // Identical benchmark source can be compiled against old and new Buffer.h.
    constexpr size_t chunk = 64 * 1024;
    constexpr int rounds = 1000;
    for (int threads : {1, 4}) {
        std::vector<double> trials;
        for (int trial = 0; trial < 5; ++trial) {
            const auto start = Clock::now();
            std::vector<std::thread> workers;
            for (int t = 0; t < threads; ++t) workers.emplace_back([] {
                std::vector<char> data(chunk, 'x');
                for (int i = 0; i < rounds; ++i) {
                    Buffer buffer;
                    for (int j = 0; j < 8; ++j) buffer.append(data.data(), data.size());
                    if (buffer.readableBytes() != chunk * 8 || buffer.peek()[chunk * 8 - 1] != 'x')
                        std::abort();
                    asm volatile("" : : "g"(buffer.peek()) : "memory");
                }
            });
            for (auto &worker : workers) worker.join();
            trials.push_back(std::chrono::duration<double>(Clock::now() - start).count());
        }
        std::sort(trials.begin(), trials.end());
        const double gib = double(threads) * rounds * chunk * 8 / (1024.0 * 1024 * 1024);
        std::printf("buffer_grow threads=%d seconds=%.6f GiB_s=%.3f median_of=5\n",
                    threads, trials[2], gib / trials[2]);
    }
}
