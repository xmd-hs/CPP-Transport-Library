#include <cstdio>
#include <string>

#include "EventLoop.h"
#include "FileTransferServer.h"
#include "InetAddress.h"
#include "Logger.h"

using transport::FileTransferServer;
using transport::TransferErrorCode;

int main(int argc, char *argv[])
{
    uint16_t port = 9000;
    std::string root = "./cloud_root";
    int ioThreads = 2;
    std::string token;

    if (argc >= 2)
    {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    if (argc >= 3)
    {
        root = argv[2];
    }
    if (argc >= 4)
    {
        ioThreads = std::stoi(argv[3]);
    }
    if (argc >= 5)
    {
        token = argv[4];
    }

    EventLoop loop;
    InetAddress addr(port, "0.0.0.0");
    FileTransferServer server(&loop, addr, root, "CloudDisk");
    server.setThreadNum(ioThreads);
    server.setIdleTimeoutSec(60.0);
    if (!token.empty())
    {
        server.setAuthToken(token);
    }
    server.setDoneCallback([](bool ok, TransferErrorCode code, const std::string &msg) {
        LOG_INFO("transfer done ok=%d code=%d msg=%s\n",
                 ok ? 1 : 0,
                 static_cast<int>(code),
                 msg.c_str());
    });
    server.start();

    std::printf("cloud_disk_server listen=%u root=%s threads=%d auth=%s\n",
                static_cast<unsigned>(port),
                root.c_str(),
                ioThreads,
                token.empty() ? "off" : "on");
    loop.loop();
    return 0;
}
