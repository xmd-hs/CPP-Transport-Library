#pragma once

#include <string>

#include "TcpServer.h"
#include "TransferCallbacks.h"
#include "TransferSession.h"
#include "noncopyable.h"

namespace transport
{

class FileTransferServer : noncopyable
{
public:
    FileTransferServer(EventLoop *loop,
                       const InetAddress &listenAddr,
                       std::string rootDir,
                       const std::string &nameArg = "FileTransferServer");

    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
    void setAuthToken(const std::string &token) { authToken_ = token; }
    void setIdleTimeoutSec(double sec) { idleTimeoutSec_ = sec; }
    void setProgressCallback(const TransferProgressCallback &cb) { progressCallback_ = cb; }
    void setDoneCallback(const TransferDoneCallback &cb) { doneCallback_ = cb; }
    void start();

private:
    void onConnection(const TcpConnectionPtr &conn);
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp);

    TcpServer server_;
    std::string rootDir_;
    std::string authToken_;
    double idleTimeoutSec_;
    TransferProgressCallback progressCallback_;
    TransferDoneCallback doneCallback_;
};

} // namespace transport
