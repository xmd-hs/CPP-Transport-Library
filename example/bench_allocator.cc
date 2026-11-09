#include <kama/MemoryPool.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

// Escape the pointer so the optimizer cannot remove malloc/free as dead work.
void touch(void *p, size_t size)
{
    if (!p) std::abort();
    auto bytes = static_cast<unsigned char *>(p);
    bytes[0] = 1;
    bytes[size - 1] = 2;
    asm volatile("" : : "g"(p) : "memory");
}

double run(bool pool, size_t size, int threads, int batches)
{
    constexpr int window = 32;
    std::atomic_int ready{0};
    std::atomic_bool start{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t)
        workers.emplace_back([&] {
            void *pointers[window];
            // Warm the allocator on the thread that will actually use it.
            for (int b = 0; b < 8; ++b) {
                for (auto &p : pointers) {
                    p = pool ? Kama_memoryPool::MemoryPool::allocate(size) : std::malloc(size);
                    touch(p, size);
                }
                for (auto p : pointers) {
                    if (pool) Kama_memoryPool::MemoryPool::deallocate(p, size);
                    else std::free(p);
                }
            }
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int b = 0; b < batches; ++b) {
                for (auto &p : pointers) {
                    p = pool ? Kama_memoryPool::MemoryPool::allocate(size) : std::malloc(size);
                    touch(p, size);
                }
                for (auto p : pointers) {
                    if (pool) Kama_memoryPool::MemoryPool::deallocate(p, size);
                    else std::free(p);
                }
            }
        });
    while (ready.load() != threads) std::this_thread::yield();
    const auto begin = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto &worker : workers) worker.join();
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
}

int main()
{
    std::puts("Warmed per-thread alloc/free; 32 live blocks/thread; median of 5 alternating trials.");
    std::puts("Times include thread join/cache teardown; endpoints touched, not full-block memory bandwidth.");
    for (int threads : {1, 4}) {
        for (size_t size : {size_t(64), size_t(256), size_t(4096), size_t(65536), size_t(262144), size_t(1048576)}) {
            const int batches = size < 65536 ? 3000 : 500;
            std::vector<double> pooled, system;
            for (int trial = 0; trial < 5; ++trial) {
                if (trial % 2 == 0) {
                    pooled.push_back(run(true, size, threads, batches));
                    system.push_back(run(false, size, threads, batches));
                } else {
                    system.push_back(run(false, size, threads, batches));
                    pooled.push_back(run(true, size, threads, batches));
                }
            }
            std::sort(pooled.begin(), pooled.end());
            std::sort(system.begin(), system.end());
            const double pairs = double(threads) * batches * 32;
            std::printf("bytes=%zu threads=%d pool_ms=%.3f system_ms=%.3f pool_Mpairs_s=%.2f system_Mpairs_s=%.2f speedup=%.2f\n",
                        size, threads, pooled[2], system[2], pairs / pooled[2] / 1000,
                        pairs / system[2] / 1000, system[2] / pooled[2]);
            Kama_memoryPool::MemoryPool::releaseUnused();
        }
    }
    std::puts("Upstream release may disable allocation statistics; zero counters do not prove zero allocations.");
}
