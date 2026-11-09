#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "Callbacks.h"
#include "TimerId.h"
#include "TransferCallbacks.h"

class Buffer;

namespace transport
{

struct DecodedFrame;

// 单连接上传/下载状态机；支持鉴权、空闲超时、传完复用。
class TransferSession : public std::enable_shared_from_this<TransferSession>
{
public:
    enum class Role
    {
        kServer,
        kClient
    };

    TransferSession(Role role,
                    const TcpConnectionPtr &conn,
                    std::string rootDir,
                    std::string authToken = std::string(),
                    double idleTimeoutSec = 60.0);
    ~TransferSession();

    void setProgressCallback(const TransferProgressCallback &cb) { progressCallback_ = cb; }
    void setDoneCallback(const TransferDoneCallback &cb) { doneCallback_ = cb; }
    void setAuthToken(const std::string &token) { authToken_ = token; }

    void onMessage(Buffer *buf);
    void onWriteComplete();
    void onClosed();
    void startIdleGuard(); // make_shared 之后调用
    void touch();          // 更新活动时间，不在热路径重建定时器

    void startUpload(const std::string &localPath,
                     const std::string &remoteRelPath,
                     uint64_t offset = 0);
    void startDownload(const std::string &remoteRelPath,
                       const std::string &localPath,
                       uint64_t offset = 0,
                       uint64_t length = 0);
    void startStat(const std::string &remoteRelPath,
                   std::function<void(bool, uint64_t, const std::string &)> cb);

    bool busy() const { return state_ != State::kIdle; }

private:
    enum class State
    {
        kIdle,
        kServerReceivingUpload,
        kServerSendingDownload,
        kClientSendingUpload,
        kClientReceivingDownload,
        kClientWaitDownloadResp,
        kClientWaitStatResp,
        kFinished
    };

    void sendError(TransferErrorCode code, const std::string &message);
    void finish(bool ok, TransferErrorCode code, const std::string &message);
    void closeFile();
    void reportProgress();
    void armIdleTimer();
    void cancelIdleTimer();
    void onIdleTimeout();

    bool checkAuth(const std::string &token) const;
    bool resolveServerPath(const std::string &rel, std::string *absOut) const;
    void handleServerFrame(const DecodedFrame &frame);
    void handleClientFrame(const DecodedFrame &frame);

    void beginServerUpload(const std::string &relPath, uint64_t offset, uint64_t fileSize);
    void beginServerDownload(const std::string &relPath, uint64_t offset, uint64_t length);
    void beginServerStat(const std::string &relPath);
    void consumeUploadData(Buffer *buf);
    void consumeDownloadData(Buffer *buf);
    void pumpClientUpload();
    void pumpServerDownload();
    void resetToIdle();

    Role role_;
    TcpConnectionPtr conn_;
    std::string rootDir_;
    std::string authToken_;
    double idleTimeoutSec_;
    State state_;
    TimerId idleTimer_;
    std::chrono::steady_clock::time_point lastActivity_;

    int fileFd_;
    uint64_t offset_;
    uint64_t fileSize_;
    uint64_t sessionTotal_;
    uint64_t transferred_;
    std::string path_;
    bool upload_;

    TransferProgressCallback progressCallback_;
    TransferDoneCallback doneCallback_;
    std::function<void(bool, uint64_t, const std::string &)> statCallback_;
    std::atomic_bool finished_;
};

} // namespace transport
