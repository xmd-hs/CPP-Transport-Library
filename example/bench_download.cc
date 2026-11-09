#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Buffer.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "TcpServer.h"
#include <kama/MemoryPool.h>

namespace
{

using Clock = std::chrono::steady_clock;

std::atomic_bool g_ready{false};
std::atomic_bool g_failed{false};

#pragma pack(push, 1)
struct DownloadRequest
{
    std::uint64_t bytes = 0;
};
#pragma pack(pop)

int connectClient(uint16_t port)
{
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // WSL 回环在瞬时并发 connect 时会直接 ECONNREFUSED，建连侧做短重试。
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (fd < 0)
        {
            return -1;
        }

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int buf = 4 * 1024 * 1024;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));

        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0)
        {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return -1;
}

bool sendAll(int fd, const void *data, std::size_t len)
{
    const char *bytes = static_cast<const char *>(data);
    std::size_t sent = 0;
    while (sent < len)
    {
        const ssize_t n = ::send(fd, bytes + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recvExact(int fd, void *data, std::size_t len)
{
    char *bytes = static_cast<char *>(data);
    std::size_t got = 0;
    while (got < len)
    {
        const ssize_t n = ::recv(fd, bytes + got, len - got, 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

struct ClientResult
{
    std::uint64_t bytes = 0;
    double seconds = 0.0;
    bool ok = false;
};

ClientResult runClients(uint16_t port, int connections, std::uint64_t bytesPerConn)
{
    std::vector<std::thread> workers;
    std::atomic_uint64_t totalBytes{0};
    std::atomic_int failures{0};
    const auto wallBegin = Clock::now();

    workers.reserve(static_cast<std::size_t>(connections));
    for (int c = 0; c < connections; ++c)
    {
        workers.emplace_back([&]() {
            const int fd = connectClient(port);
            if (fd < 0)
            {
                failures.fetch_add(1);
                return;
            }

            DownloadRequest request;
            request.bytes = bytesPerConn;
            if (!sendAll(fd, &request, sizeof(request)))
            {
                failures.fetch_add(1);
                ::close(fd);
                return;
            }

            std::vector<char> buffer(256 * 1024);
            std::uint64_t remaining = bytesPerConn;
            std::uint64_t got = 0;
            while (remaining > 0)
            {
                const std::size_t want = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, buffer.size()));
                if (!recvExact(fd, buffer.data(), want))
                {
                    failures.fetch_add(1);
                    ::close(fd);
                    return;
                }
                remaining -= want;
                got += want;
            }
            ::close(fd);
            totalBytes.fetch_add(got);
        });
    }

    for (auto &worker : workers)
    {
        worker.join();
    }

    ClientResult result;
    result.bytes = totalBytes.load();
    result.seconds = std::chrono::duration<double>(Clock::now() - wallBegin).count();
    result.ok = failures.load() == 0 &&
                result.bytes == bytesPerConn * static_cast<std::uint64_t>(connections);
    return result;
}

void printScenario(const char *name, const ClientResult &result)
{
    const double mib = result.bytes / (1024.0 * 1024.0);
    const double mibPerSec = result.seconds > 0.0 ? (mib / result.seconds) : 0.0;
    std::printf("%-28s  ok=%d  bytes=%.1f MiB  wall=%.3fs  throughput=%.1f MiB/s\n",
                name,
                result.ok ? 1 : 0,
                mib,
                result.seconds,
                mibPerSec);
}

thread_local std::vector<char> t_sendChunk;

void pumpDownload(const TcpConnectionPtr &conn, const std::shared_ptr<std::uint64_t> &left)
{
    constexpr std::size_t kChunk = 256 * 1024;
    // 每次回调最多再灌约 1MiB，配合 writeComplete 合并，避免一次打爆 outputBuffer。
    constexpr std::size_t kBatch = 1024 * 1024;

    if (t_sendChunk.size() != kChunk)
    {
        t_sendChunk.assign(kChunk, 'D');
    }

    if (*left == 0)
    {
        if (conn->outputBufferBytes() == 0 && !conn->isWriting())
        {
            conn->setWriteCompleteCallback(WriteCompleteCallback());
            conn->shutdown();
        }
        return;
    }

    std::size_t budget = kBatch;
    while (*left > 0 && budget > 0 && conn->outputBufferBytes() < kBatch)
    {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(*left,
                                    std::min<std::uint64_t>(t_sendChunk.size(), budget)));
        *left -= n;
        budget -= n;
        conn->send(t_sendChunk.data(), n);
    }
}

void handleMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    if (buf->readableBytes() < sizeof(DownloadRequest))
    {
        return;
    }

    DownloadRequest request;
    std::memcpy(&request, buf->peek(), sizeof(request));
    buf->retrieve(sizeof(request));

    auto left = std::make_shared<std::uint64_t>(request.bytes);
    conn->setHighWaterMarkCallback(
        [](const TcpConnectionPtr &, size_t) {
            // 水位只作观测；真正节流靠 outputBufferBytes + writeComplete。
        },
        4 * 1024 * 1024);
    conn->setWriteCompleteCallback(
        [left](const TcpConnectionPtr &c) { pumpDownload(c, left); });
    pumpDownload(conn, left);
}

} // namespace

int main(int argc, char *argv[])
{
    uint16_t port = 18080;
    int ioThreads = 4;
    if (argc >= 2)
    {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    if (argc >= 3)
    {
        ioThreads = std::stoi(argv[2]);
    }

    EventLoop loop;
    InetAddress addr(port, "127.0.0.1");
    TcpServer server(&loop, addr, "DownloadCoreBench");
    server.setThreadNum(ioThreads);
    server.setConnectionCallback([](const TcpConnectionPtr &) {});
    server.setMessageCallback(handleMessage);
    server.start();
    g_ready.store(true);

    std::thread runner([&]() {
        while (!g_ready.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::printf("=== download-core one-way stream (ioThreads=%d port=%u) ===\n",
                    ioThreads,
                    static_cast<unsigned>(port));

        struct Scenario
        {
            const char *name;
            int connections;
            std::uint64_t bytesPerConn;
        };

        const Scenario scenarios[] = {
            {"1c 256MiB", 1, 256ull * 1024 * 1024},
            {"4c 128MiB", 4, 128ull * 1024 * 1024},
            {"8c 64MiB", 8, 64ull * 1024 * 1024},
            {"16c 32MiB", 16, 32ull * 1024 * 1024},
            {"32c 16MiB", 32, 16ull * 1024 * 1024},
            {"64c 8MiB", 64, 8ull * 1024 * 1024},
        };

        for (const Scenario &scenario : scenarios)
        {
            // 等上一轮连接在服务端回收完，避免场景之间互相抢建连。
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const ClientResult result =
                runClients(port, scenario.connections, scenario.bytesPerConn);
            printScenario(scenario.name, result);
            if (!result.ok)
            {
                g_failed.store(true);
            }
            const auto stats = Kama_memoryPool::MemoryPool::stats();
            std::printf("  pool reserved=%.2f MiB cachedPages=%.2f MiB\n",
                        stats.reservedBytes / (1024.0 * 1024.0),
                        stats.cachedPageBytes / (1024.0 * 1024.0));
            Kama_memoryPool::MemoryPool::releaseUnused();
        }

        loop.quit();
    });

    loop.loop();
    runner.join();
    return g_failed.load() ? 1 : 0;
}
