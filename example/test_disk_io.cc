#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "DiskIoPool.h"
#include "EventLoop.h"
#include "XmdMemory.h"

int main()
{
    bool oversizedWriteRejected = false;
    {
        EventLoop loop;
        auto payload = makePoolShared<PoolString>(
            transport::DiskIoPool::kMaxOutstandingBytes + 1, 'x');
        oversizedWriteRejected = !transport::DiskIoPool::instance().pwrite(
            &loop, -1, payload, 0, [](ssize_t, int) {});
    }

    bool aggregateByteLimitEnforced = false;
    {
        EventLoop loop;
        auto &pool = transport::DiskIoPool::instance();
        auto firstHalf = makePoolShared<PoolString>(
            transport::DiskIoPool::kMaxOutstandingBytes / 2, 'y');
        auto secondHalf = makePoolShared<PoolString>(
            transport::DiskIoPool::kMaxOutstandingBytes / 2, 'y');
        const bool firstAccepted = pool.pwrite(&loop, -1, firstHalf, 0, [](ssize_t, int) {});
        const bool secondAccepted = pool.pwrite(&loop, -1, secondHalf, 0, [](ssize_t, int) {});
        const bool thirdRejected = !pool.pwrite(
            &loop, -1, makePoolShared<PoolString>(1, 'z'), 0, [](ssize_t, int) {});
        aggregateByteLimitEnforced = firstAccepted && secondAccepted && thirdRejected;
    }

    bool operationLimitEnforced = false;
    {
        EventLoop loop;
        auto &pool = transport::DiskIoPool::instance();
        size_t accepted = 0;
        for (; accepted < transport::DiskIoPool::kMaxOutstandingOps; ++accepted)
        {
            if (!pool.fsync(&loop, -1, [](int) {}))
            {
                break;
            }
        }
        operationLimitEnforced =
            accepted == transport::DiskIoPool::kMaxOutstandingOps &&
            !pool.fsync(&loop, -1, [](int) {});
    }

    std::atomic<bool> callbackRan(false);
    std::thread worker;
    const auto start = std::chrono::steady_clock::now();
    {
        EventLoop loop;
        auto operation = loop.beginAsyncOperation();
        if (!operation)
        {
            return 1;
        }
        worker = std::thread([operation = std::move(operation), &callbackRan]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            operation.post([&callbackRan] { callbackRan.store(true); });
        });
    } // Shutdown drains the lease before closing the eventfd.
    const auto elapsed = std::chrono::steady_clock::now() - start;
    worker.join();

    const bool loopWaited = elapsed >= std::chrono::milliseconds(80);
    const bool callbackDiscarded = !callbackRan.load();

    constexpr int kCoalescedCallbacks = 128;
    std::atomic<int> completedCallbacks(0);
    bool coalescedCallbacksDelivered = false;
    {
        EventLoop loop;
        std::vector<EventLoop::AsyncOperation> operations;
        operations.reserve(kCoalescedCallbacks);
        for (int i = 0; i < kCoalescedCallbacks; ++i)
        {
            operations.emplace_back(loop.beginAsyncOperation());
            if (!operations.back())
            {
                return 1;
            }
        }
        std::vector<std::thread> posters;
        for (int posterIndex = 0; posterIndex < 4; ++posterIndex)
        {
            posters.emplace_back([&, posterIndex] {
                for (int i = posterIndex; i < kCoalescedCallbacks; i += 4)
                {
                    auto &operation = operations[static_cast<std::size_t>(i)];
                    if (!operation.post([&loop, &completedCallbacks, kCoalescedCallbacks] {
                            if (completedCallbacks.fetch_add(1) + 1 == kCoalescedCallbacks)
                            {
                                loop.quit();
                            }
                        }))
                    {
                        loop.queueInLoop([&loop] { loop.quit(); });
                    }
                }
            });
        }
        loop.loop();
        for (auto &poster : posters)
        {
            poster.join();
        }
        coalescedCallbacksDelivered = completedCallbacks.load() == kCoalescedCallbacks;
    }

    std::printf("oversized disk write rejected: %s; aggregate byte limit enforced: %s; "
                "operation limit enforced: %s; shutdown drains worker: %s; "
                "late callback discarded safely: %s; coalesced callbacks delivered: %s\n",
                oversizedWriteRejected ? "PASS" : "FAIL",
                aggregateByteLimitEnforced ? "PASS" : "FAIL",
                operationLimitEnforced ? "PASS" : "FAIL",
                loopWaited ? "PASS" : "FAIL",
                callbackDiscarded ? "PASS" : "FAIL",
                coalescedCallbacksDelivered ? "PASS" : "FAIL");
    return oversizedWriteRejected && aggregateByteLimitEnforced &&
                   operationLimitEnforced && loopWaited && callbackDiscarded &&
                   coalescedCallbacksDelivered
               ? 0
               : 1;
}
