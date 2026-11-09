#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "InetAddress.h"
#include "TcpClient.h"
#include "TransferCallbacks.h"
#include "TransferSession.h"
#include "noncopyable.h"

namespace transport
{

class FileTransferClient : noncopyable
{
public:
    FileTransferClient(EventLoop *loop,
                       const InetAddress &serverAddr,
                       const std::string &nameArg = "FileTransferClient");

    void setProgressCallback(const TransferProgressCallback &cb) { progressCallback_ = cb; }
    void setAuthToken(const std::string &token) { authToken_ = token; }
    void setIdleTimeoutSec(double sec) { idleTimeoutSec_ = sec; }
    void enableRetry() { client_.enableRetry(); }

    void connect();
    void disconnect();
    bool connected() const;

    void upload(const std::string &localPath,
                const std::string &remoteRelPath,
                uint64_t offset = 0,
                TransferDoneCallback done = TransferDoneCallback());

    void download(const std::string &remoteRelPath,
                  const std::string &localPath,
                  uint64_t offset = 0,
                  uint64_t length = 0,
                  TransferDoneCallback done = TransferDoneCallback());

    // 多分片并行下载：先 Stat 再按分片建多个连接拉取。
    void downloadParallel(const std::string &remoteRelPath,
                          const std::string &localPath,
                          int parts,
                          TransferDoneCallback done = TransferDoneCallback());

    void stat(const std::string &remoteRelPath,
              std::function<void(bool ok, uint64_t size, const std::string &msg)> cb);

    bool waitDone(int timeoutMs, bool *okOut, std::string *messageOut);

private:
    void onConnection(const TcpConnectionPtr &conn);
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp);
    void bindSession(const TcpConnectionPtr &conn);
    void runTransfer(std::function<void(const std::shared_ptr<TransferSession> &)> fn,
                     TransferDoneCallback done);
    void notifyDone(bool ok, TransferErrorCode code, const std::string &msg);

    EventLoop *loop_;
    InetAddress serverAddr_;
    std::string name_;
    TcpClient client_;
    std::mutex mutex_;
    std::shared_ptr<TransferSession> session_;
    TransferProgressCallback progressCallback_;
    std::string authToken_;
    double idleTimeoutSec_;

    std::mutex doneMutex_;
    std::condition_variable doneCv_;
    bool doneReady_;
    bool lastOk_;
    std::string lastMessage_;
};

} // namespace transport
