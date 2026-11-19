#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <vector>

#include "EventLoop.h"
#include "XmdMemory.h"
#include "noncopyable.h"

namespace transport
{

// Small process-wide pool for blocking disk pwrite/fsync off the reactor.
// MUDUO_DISK_IO_WORKERS selects 1..16 workers (default 2) on first use.
class DiskIoPool : noncopyable
{
public:
    static constexpr size_t kMaxOutstandingBytes = 64 * 1024 * 1024;
    static constexpr size_t kMaxOutstandingOps = 1024;

    using WriteDone = std::function<void(ssize_t n, int err)>;
    using SyncDone = std::function<void(int err)>;

    static DiskIoPool &instance();

    bool pwrite(EventLoop *loop,
                int fd,
                std::shared_ptr<PoolString> data,
                off_t offset,
                WriteDone done);

    bool fsync(EventLoop *loop, int fd, SyncDone done);

private:
    enum class OpType
    {
        kPwrite,
        kFsync
    };

    struct OutstandingReservation;
    struct CapacityState;

    struct Job
    {
        OpType type = OpType::kPwrite;
        EventLoop::AsyncOperation loopOperation;
        int fd = -1;
        off_t offset = 0;
        size_t bytes = 0;
        std::shared_ptr<OutstandingReservation> reservation;
        std::shared_ptr<PoolString> data;
        WriteDone writeDone;
        SyncDone syncDone;
    };

    struct OutstandingReservation
    {
        OutstandingReservation(std::shared_ptr<CapacityState> capacityArg, size_t bytesArg)
            : capacity(std::move(capacityArg)), bytes(bytesArg), armed(false)
        {
        }
        ~OutstandingReservation();

        std::shared_ptr<CapacityState> capacity;
        size_t bytes;
        bool armed;
    };

    struct CapacityState
    {
        std::mutex mutex;
        size_t bytes = 0;
        size_t ops = 0;
    };

    explicit DiskIoPool(size_t workers = 2);
    ~DiskIoPool();

    void workerLoop();
    bool enqueue(Job job);
    static void releaseOutstanding(const std::shared_ptr<CapacityState> &capacity, size_t bytes);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Job> queue_;
    std::shared_ptr<CapacityState> capacity_;
    bool stopping_;
    std::vector<std::thread> workers_;
};

} // namespace transport
