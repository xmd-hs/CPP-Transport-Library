#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "EventLoop.h"
#include "EventLoopThread.h"
#include "FileTransferClient.h"
#include "InetAddress.h"

using transport::FileTransferClient;
using transport::TransferProgress;

namespace
{

void usage(const char *prog)
{
    std::printf("usage:\n");
    std::printf("  %s upload    <host> <port> <localPath> <remoteRelPath> [offset] [token]\n", prog);
    std::printf("  %s download  <host> <port> <remoteRelPath> <localPath> [offset] [length] [token]\n", prog);
    std::printf("  %s pdownload <host> <port> <remoteRelPath> <localPath> <parts> [token]\n", prog);
}

std::string optionalToken(int argc, char *argv[], int index)
{
    if (argc > index)
    {
        return argv[index];
    }
    return {};
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 6)
    {
        usage(argv[0]);
        return 1;
    }

    const std::string cmd = argv[1];
    const std::string host = argv[2];
    const uint16_t port = static_cast<uint16_t>(std::stoi(argv[3]));

    EventLoopThread loopThread;
    EventLoop *loop = loopThread.startLoop();

    InetAddress serverAddr(port, host);
    FileTransferClient client(loop, serverAddr, "CloudDiskClient");
    client.enableRetry();
    client.setProgressCallback([last = std::chrono::steady_clock::time_point{}](const TransferProgress &p) mutable {
        if (p.total == 0)
        {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (p.transferred < p.total && now - last < std::chrono::milliseconds(100))
        {
            return;
        }
        last = now;
        const double pct = 100.0 * static_cast<double>(p.transferred) / static_cast<double>(p.total);
        std::printf("\r%s %s %.1f%% (%llu/%llu)",
                    p.upload ? "upload" : "download",
                    p.path.c_str(),
                    pct,
                    static_cast<unsigned long long>(p.transferred),
                    static_cast<unsigned long long>(p.total));
        std::fflush(stdout);
    });

    client.connect();
    for (int i = 0; i < 500 && !client.connected(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!client.connected())
    {
        std::fprintf(stderr, "connect failed\n");
        return 1;
    }

    const auto begin = std::chrono::steady_clock::now();
    if (cmd == "upload")
    {
        const std::string localPath = argv[4];
        const std::string remotePath = argv[5];
        uint64_t offset = 0;
        int tokenIdx = 6;
        if (argc >= 7 && argv[6][0] >= '0' && argv[6][0] <= '9')
        {
            offset = std::stoull(argv[6]);
            tokenIdx = 7;
        }
        client.setAuthToken(optionalToken(argc, argv, tokenIdx));
        client.upload(localPath, remotePath, offset);
    }
    else if (cmd == "download")
    {
        const std::string remotePath = argv[4];
        const std::string localPath = argv[5];
        uint64_t offset = 0;
        uint64_t length = 0;
        int tokenIdx = 6;
        if (argc >= 7 && argv[6][0] >= '0' && argv[6][0] <= '9')
        {
            offset = std::stoull(argv[6]);
            tokenIdx = 7;
            if (argc >= 8 && argv[7][0] >= '0' && argv[7][0] <= '9')
            {
                length = std::stoull(argv[7]);
                tokenIdx = 8;
            }
        }
        client.setAuthToken(optionalToken(argc, argv, tokenIdx));
        client.download(remotePath, localPath, offset, length);
    }
    else if (cmd == "pdownload")
    {
        if (argc < 7)
        {
            usage(argv[0]);
            return 1;
        }
        const std::string remotePath = argv[4];
        const std::string localPath = argv[5];
        const int parts = std::stoi(argv[6]);
        client.setAuthToken(optionalToken(argc, argv, 7));
        client.downloadParallel(remotePath, localPath, parts);
    }
    else
    {
        usage(argv[0]);
        return 1;
    }

    bool ok = false;
    std::string message;
    if (!client.waitDone(600000, &ok, &message))
    {
        std::fprintf(stderr, "\ntimeout waiting transfer\n");
        return 1;
    }

    const double sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("\ndone ok=%d msg=%s wall=%.3fs\n", ok ? 1 : 0, message.c_str(), sec);
    client.disconnect();
    return ok ? 0 : 2;
}
