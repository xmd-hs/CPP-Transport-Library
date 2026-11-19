#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>

#include "Callbacks.h"
#include "Protocol.h"
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
    void reportProgress(bool force = false);
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

    void sendFrame(MessageType type,
                   const std::string &extra,
                   const void *body,
                   size_t bodyLen,
                   uint64_t offset,
                   uint64_t fileSize,
                   uint16_t flags = kFlagNone);
    void queueDiskWrite(const char *data, size_t n);
    void onDiskWriteDone(size_t expected, ssize_t wrote, int err);
    void completeAfterDisk();
    void updateReadPressure();

    Role role_;
    TcpConnectionPtr conn_;
    std::string rootDir_;
    std::string authToken_;
    double idleTimeoutSec_;
    State state_;
    TimerId idleTimer_;
    std::chrono::steady_clock::time_point lastActivity_;
    std::chrono::steady_clock::time_point lastProgressReport_;

    int fileFd_;
    uint64_t offset_;
    uint64_t fileSize_;
    uint64_t sessionTotal_;
    uint64_t transferred_;
    uint64_t nextWriteOffset_;
    size_t pendingDiskBytes_;
    int pendingDiskOps_;
    bool diskClosing_;
    bool readingPaused_;
    std::string path_;
    bool upload_;

    TransferProgressCallback progressCallback_;
    TransferDoneCallback doneCallback_;
    std::function<void(bool, uint64_t, const std::string &)> statCallback_;
    std::atomic_bool finished_;

    static constexpr size_t kDiskHighWater = 4 * 1024 * 1024;
    static constexpr size_t kDiskLowWater = 1 * 1024 * 1024;
};

} // namespace transport
