#include <xmd/MemoryPool.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Buffer.h"
#include "XmdMemory.h"

namespace
{

using Clock = std::chrono::steady_clock;

double millis(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

void printRow(const char *name, double poolMs, double mallocMs, std::uint64_t ops)
{
    const double safePool = poolMs <= 0.0 ? 1e-6 : poolMs;
    const double safeMalloc = mallocMs <= 0.0 ? 1e-6 : mallocMs;
    const double poolOps = ops / (safePool / 1000.0);
    const double mallocOps = ops / (safeMalloc / 1000.0);
    const double speedup = safeMalloc / safePool;
    std::printf("%-28s  pool=%8.3f ms (%8.2f Mops/s)  malloc=%8.3f ms (%8.2f Mops/s)  x%.2f\n",
                name,
                poolMs,
                poolOps / 1e6,
                mallocMs,
                mallocOps / 1e6,
                speedup);
}

void touchBlock(void *pointer, std::size_t size)
{
    auto *bytes = static_cast<unsigned char *>(pointer);
    bytes[0] = 1;
    bytes[size - 1] = 2;
    asm volatile("" : : "g"(pointer) : "memory");
}

double benchPoolSized(std::size_t size, int rounds)
{
    std::vector<void *> pointers(static_cast<std::size_t>(rounds));
    const auto begin = Clock::now();
    for (int i = 0; i < rounds; ++i)
    {
        void *pointer = xmd_memoryPool::MemoryPool::allocate(size);
        touchBlock(pointer, size);
        pointers[static_cast<std::size_t>(i)] = pointer;
    }
    for (int i = 0; i < rounds; ++i)
    {
        xmd_memoryPool::MemoryPool::deallocate(pointers[static_cast<std::size_t>(i)], size);
    }
    return millis(begin, Clock::now());
}

double benchMallocSized(std::size_t size, int rounds)
{
    std::vector<void *> pointers(static_cast<std::size_t>(rounds));
    const auto begin = Clock::now();
    for (int i = 0; i < rounds; ++i)
    {
        void *pointer = std::malloc(size);
        touchBlock(pointer, size);
        pointers[static_cast<std::size_t>(i)] = pointer;
    }
    for (int i = 0; i < rounds; ++i)
    {
        std::free(pointers[static_cast<std::size_t>(i)]);
    }
    return millis(begin, Clock::now());
}

double benchPoolChurn(std::size_t size, int rounds)
{
    const auto begin = Clock::now();
    for (int i = 0; i < rounds; ++i)
    {
        void *pointer = xmd_memoryPool::MemoryPool::allocate(size);
        touchBlock(pointer, size);
        xmd_memoryPool::MemoryPool::deallocate(pointer, size);
    }
    return millis(begin, Clock::now());
}

double benchMallocChurn(std::size_t size, int rounds)
{
    const auto begin = Clock::now();
    for (int i = 0; i < rounds; ++i)
    {
        void *pointer = std::malloc(size);
        touchBlock(pointer, size);
        std::free(pointer);
    }
    return millis(begin, Clock::now());
}

double benchBufferGrow(std::size_t chunk, int rounds)
{
    std::vector<char> payload(chunk, 'd');
    const auto begin = Clock::now();
    for (int i = 0; i < rounds; ++i)
    {
        Buffer buffer(1024);
        for (int part = 0; part < 8; ++part)
        {
            buffer.append(payload.data(), payload.size());
        }
        if (buffer.readableBytes() != chunk * 8)
        {
            std::abort();
        }
        buffer.retrieveAll();
    }
    return millis(begin, Clock::now());
}

} // namespace

int main()
{
    std::printf("=== download-core alloc bench (xmdMemoryPool 2.0.0) ===\n");
    std::puts("Legacy single-thread workload; use bench_allocator for repeated multithread comparisons.");

    struct Case
    {
        const char *name;
        std::size_t size;
        int rounds;
    };

    const Case cases[] = {
        {"4KiB burst live", 4 * 1024, 50000},
        {"64KiB burst live", 64 * 1024, 2000},
        {"256KiB burst live", 256 * 1024, 512},
        {"1MiB burst live", 1024 * 1024, 128},
        {"4KiB churn", 4 * 1024, 200000},
        {"64KiB churn", 64 * 1024, 50000},
        {"256KiB churn", 256 * 1024, 10000},
    };

    for (const Case &item : cases)
    {
        double poolMs = 0.0;
        double mallocMs = 0.0;
        if (std::strstr(item.name, "churn") != nullptr)
        {
            poolMs = benchPoolChurn(item.size, item.rounds);
            mallocMs = benchMallocChurn(item.size, item.rounds);
        }
        else
        {
            poolMs = benchPoolSized(item.size, item.rounds);
            mallocMs = benchMallocSized(item.size, item.rounds);
        }
        printRow(item.name, poolMs, mallocMs, static_cast<std::uint64_t>(item.rounds));
        xmd_memoryPool::MemoryPool::releaseUnused();
    }

    const double bufferMs = benchBufferGrow(64 * 1024, 2000);
    const double bufferGb =
        (2000.0 * 8.0 * 64.0 * 1024.0) / (bufferMs / 1000.0) / (1024.0 * 1024.0 * 1024.0);
    std::printf("%-28s  %.2f ms  append/retrieve throughput %.2f GiB/s\n",
                "Buffer 64KiB x8 grow",
                bufferMs,
                bufferGb);

    const auto stats = xmd_memoryPool::MemoryPool::stats();
    std::printf("pool reservedBytes=%llu cachedPageBytes=%llu\n",
                static_cast<unsigned long long>(stats.reservedBytes),
                static_cast<unsigned long long>(stats.cachedPageBytes));
    return 0;
}
