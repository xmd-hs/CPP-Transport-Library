#include "DiskIoPool.h"

#include <errno.h>
#include <cstdlib>
#include <new>
#include <unistd.h>

namespace transport
{

constexpr size_t DiskIoPool::kMaxOutstandingBytes;
constexpr size_t DiskIoPool::kMaxOutstandingOps;

DiskIoPool::OutstandingReservation::~OutstandingReservation()
{
    if (armed && capacity)
    {
        DiskIoPool::releaseOutstanding(capacity, bytes);
    }
}

DiskIoPool &DiskIoPool::instance()
{
    static DiskIoPool pool([] {
        const char *value = std::getenv("MUDUO_DISK_IO_WORKERS");
        if (!value || *value == '\0')
        {
            return size_t(2);
        }

        char *end = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 16)
        {
            return size_t(2);
        }
        return static_cast<size_t>(parsed);
    }());
    return pool;
}

DiskIoPool::DiskIoPool(size_t workers)
    : capacity_(std::make_shared<CapacityState>()), stopping_(false)
{
    if (workers == 0)
    {
        workers = 1;
    }
    workers_.reserve(workers);
    for (size_t i = 0; i < workers; ++i)
    {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

DiskIoPool::~DiskIoPool()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto &t : workers_)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
}

bool DiskIoPool::enqueue(Job job)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> capacityLock(capacity_->mutex);
        if (stopping_ || capacity_->ops >= kMaxOutstandingOps ||
            job.bytes > kMaxOutstandingBytes - capacity_->bytes)
        {
            return false;
        }
        try
        {
            queue_.push(std::move(job));
        }
        catch (const std::bad_alloc &)
        {
            return false;
        }
        queue_.back().reservation->armed = true;
        // job has been moved into the queue. Read the reserved size from the
        // queued instance instead of relying on moved-from scalar members.
        capacity_->bytes += queue_.back().bytes;
        ++capacity_->ops;
    }
    cv_.notify_one();
    return true;
}

void DiskIoPool::releaseOutstanding(const std::shared_ptr<CapacityState> &capacity, size_t bytes)
{
    std::lock_guard<std::mutex> lock(capacity->mutex);
    if (capacity->ops > 0)
    {
        --capacity->ops;
    }
    if (capacity->bytes >= bytes)
    {
        capacity->bytes -= bytes;
    }
    else
    {
        capacity->bytes = 0;
    }
}

bool DiskIoPool::pwrite(EventLoop *loop,
                        int fd,
                        std::shared_ptr<PoolString> data,
                        off_t offset,
                        WriteDone done)
{
    if (!loop || (data && data->size() > kMaxOutstandingBytes))
    {
        return false;
    }
    EventLoop::AsyncOperation loopOperation = loop->beginAsyncOperation();
    if (!loopOperation)
    {
        return false;
    }
    std::shared_ptr<OutstandingReservation> reservation;
    try
    {
        reservation = std::make_shared<OutstandingReservation>(capacity_, data ? data->size() : 0);
    }
    catch (const std::bad_alloc &)
    {
        return false;
    }
    Job job;
    job.type = OpType::kPwrite;
    job.loopOperation = std::move(loopOperation);
    job.fd = fd;
    job.offset = offset;
    job.bytes = data ? data->size() : 0;
    job.reservation = std::move(reservation);
    job.data = std::move(data);
    job.writeDone = std::move(done);
    return enqueue(std::move(job));
}

bool DiskIoPool::fsync(EventLoop *loop, int fd, SyncDone done)
{
    if (!loop)
    {
        return false;
    }
    EventLoop::AsyncOperation loopOperation = loop->beginAsyncOperation();
    if (!loopOperation)
    {
        return false;
    }
    std::shared_ptr<OutstandingReservation> reservation;
    try
    {
        reservation = std::make_shared<OutstandingReservation>(capacity_, 0);
    }
    catch (const std::bad_alloc &)
    {
        return false;
    }
    Job job;
    job.type = OpType::kFsync;
    job.loopOperation = std::move(loopOperation);
    job.fd = fd;
    job.reservation = std::move(reservation);
    job.syncDone = std::move(done);
    return enqueue(std::move(job));
}

void DiskIoPool::workerLoop()
{
    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty())
            {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop();
        }

        if (job.type == OpType::kPwrite)
        {
            ssize_t total = 0;
            int err = 0;
            const char *p = job.data ? job.data->data() : nullptr;
            size_t left = job.data ? job.data->size() : 0;
            off_t off = job.offset;
            while (left > 0)
            {
                const ssize_t n = ::pwrite(job.fd, p + total, left, off);
                if (n < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    err = errno;
                    total = -1;
                    break;
                }
                if (n == 0)
                {
                    err = EIO;
                    total = -1;
                    break;
                }
                total += n;
                off += n;
                left -= static_cast<size_t>(n);
            }
            job.data.reset();
            if (job.writeDone)
            {
                auto done = std::move(job.writeDone);
                auto reservation = job.reservation;
                job.loopOperation.post([done, reservation, total, err]() { done(total, err); });
            }
            job.loopOperation = EventLoop::AsyncOperation();
        }
        else
        {
            int err = 0;
            while (::fsync(job.fd) < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                err = errno;
                break;
            }
            if (job.syncDone)
            {
                auto done = std::move(job.syncDone);
                auto reservation = job.reservation;
                job.loopOperation.post([done, reservation, err]() { done(err); });
            }
            job.loopOperation = EventLoop::AsyncOperation();
        }
    }
}

} // namespace transport
