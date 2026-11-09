#include "FileTransferClient.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

#include "EventLoop.h"
#include "Logger.h"

namespace transport
{

FileTransferClient::FileTransferClient(EventLoop *loop,
                                       const InetAddress &serverAddr,
                                       const std::string &nameArg)
    : loop_(loop)
    , serverAddr_(serverAddr)
    , name_(nameArg)
    , client_(loop, serverAddr, nameArg)
    , idleTimeoutSec_(120.0)
    , doneReady_(false)
    , lastOk_(false)
{
    client_.setConnectionCallback(
        std::bind(&FileTransferClient::onConnection, this, std::placeholders::_1));
    client_.setMessageCallback(
        std::bind(&FileTransferClient::onMessage,
                  this,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  std::placeholders::_3));
}

void FileTransferClient::connect()
{
    client_.connect();
}

void FileTransferClient::disconnect()
{
    client_.disconnect();
}

bool FileTransferClient::connected() const
{
    TcpConnectionPtr conn = client_.connection();
    return conn && conn->connected();
}

void FileTransferClient::bindSession(const TcpConnectionPtr &conn)
{
    auto session = makePoolShared<TransferSession>(
        TransferSession::Role::kClient, conn, std::string(), authToken_, idleTimeoutSec_);
    if (progressCallback_)
    {
        session->setProgressCallback(progressCallback_);
    }
    session->startIdleGuard();
    conn->setContext(session);
    std::lock_guard<std::mutex> lock(mutex_);
    session_ = session;
}

void FileTransferClient::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        bindSession(conn);
        LOG_INFO("FileTransferClient connected %s\n", conn->peerAddress().toIpPort().c_str());
    }
    else
    {
        auto session = conn->getContext<TransferSession>();
        if (session)
        {
            session->onClosed();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        session_.reset();
    }
}

void FileTransferClient::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    auto session = conn->getContext<TransferSession>();
    if (!session)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session = session_;
    }
    if (session)
    {
        session->onMessage(buf);
    }
}

void FileTransferClient::notifyDone(bool ok, TransferErrorCode code, const std::string &msg)
{
    std::lock_guard<std::mutex> lock(doneMutex_);
    doneReady_ = true;
    lastOk_ = ok;
    lastMessage_ = msg.empty() ? std::to_string(static_cast<int>(code)) : msg;
    doneCv_.notify_all();
}

void FileTransferClient::runTransfer(std::function<void(const std::shared_ptr<TransferSession> &)> fn,
                                     TransferDoneCallback done)
{
    {
        std::lock_guard<std::mutex> lock(doneMutex_);
        doneReady_ = false;
        lastOk_ = false;
        lastMessage_.clear();
    }

    loop_->runInLoop([this, fn, done]() {
        std::shared_ptr<TransferSession> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session = session_;
        }
        if (!session)
        {
            if (done)
            {
                done(false, TransferErrorCode::kIOError, "not connected");
            }
            notifyDone(false, TransferErrorCode::kIOError, "not connected");
            return;
        }

        session->setAuthToken(authToken_);
        session->setDoneCallback([this, done](bool ok, TransferErrorCode code, const std::string &msg) {
            if (done)
            {
                done(ok, code, msg);
            }
            notifyDone(ok, code, msg);
        });
        fn(session);
    });
}

void FileTransferClient::upload(const std::string &localPath,
                                const std::string &remoteRelPath,
                                uint64_t offset,
                                TransferDoneCallback done)
{
    runTransfer(
        [localPath, remoteRelPath, offset](const std::shared_ptr<TransferSession> &session) {
            session->startUpload(localPath, remoteRelPath, offset);
        },
        std::move(done));
}

void FileTransferClient::download(const std::string &remoteRelPath,
                                  const std::string &localPath,
                                  uint64_t offset,
                                  uint64_t length,
                                  TransferDoneCallback done)
{
    runTransfer(
        [remoteRelPath, localPath, offset, length](const std::shared_ptr<TransferSession> &session) {
            session->startDownload(remoteRelPath, localPath, offset, length);
        },
        std::move(done));
}

void FileTransferClient::stat(const std::string &remoteRelPath,
                              std::function<void(bool, uint64_t, const std::string &)> cb)
{
    loop_->runInLoop([this, remoteRelPath, cb]() {
        std::shared_ptr<TransferSession> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session = session_;
        }
        if (!session)
        {
            if (cb)
            {
                cb(false, 0, "not connected");
            }
            return;
        }
        session->setAuthToken(authToken_);
        session->startStat(remoteRelPath, cb);
    });
}

void FileTransferClient::downloadParallel(const std::string &remoteRelPath,
                                          const std::string &localPath,
                                          int parts,
                                          TransferDoneCallback done)
{
    if (parts <= 1)
    {
        download(remoteRelPath, localPath, 0, 0, std::move(done));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(doneMutex_);
        doneReady_ = false;
        lastOk_ = false;
        lastMessage_.clear();
    }

    // 先用主连接 Stat，再为每个分片临时建 TcpClient。
    stat(remoteRelPath, [this, remoteRelPath, localPath, parts, done](bool ok, uint64_t size, const std::string &msg) {
        if (!ok || size == 0)
        {
            if (done)
            {
                done(false, TransferErrorCode::kNotFound, msg.empty() ? "stat failed" : msg);
            }
            notifyDone(false, TransferErrorCode::kNotFound, msg.empty() ? "stat failed" : msg);
            return;
        }

        const int fd = ::open(localPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(size)) < 0)
        {
            if (fd >= 0)
            {
                ::close(fd);
            }
            if (done)
            {
                done(false, TransferErrorCode::kIOError, "prepare local failed");
            }
            notifyDone(false, TransferErrorCode::kIOError, "prepare local failed");
            return;
        }
        ::close(fd);

        struct PartState
        {
            std::unique_ptr<TcpClient> client;
            std::shared_ptr<TransferSession> session;
            bool finished = false;
            bool ok = false;
            std::string message;
        };

        auto partsState = std::make_shared<std::vector<PartState>>(static_cast<size_t>(parts));
        auto remain = std::make_shared<std::atomic_int>(parts);
        auto failed = std::make_shared<std::atomic_bool>(false);

        const uint64_t chunk = (size + static_cast<uint64_t>(parts) - 1) / static_cast<uint64_t>(parts);

        for (int i = 0; i < parts; ++i)
        {
            const uint64_t offset = static_cast<uint64_t>(i) * chunk;
            if (offset >= size)
            {
                if (remain->fetch_sub(1) == 1)
                {
                    if (done)
                    {
                        done(true, TransferErrorCode::kOk, "download parallel done");
                    }
                    notifyDone(true, TransferErrorCode::kOk, "download parallel done");
                }
                continue;
            }
            const uint64_t length = std::min(chunk, size - offset);
            const size_t idx = static_cast<size_t>(i);
            (*partsState)[idx].client.reset(
                new TcpClient(loop_, serverAddr_, name_ + "-p" + std::to_string(i)));

            (*partsState)[idx].client->setConnectionCallback(
                [this, partsState, idx, remoteRelPath, localPath, offset, length, remain, failed, done](
                    const TcpConnectionPtr &conn) {
                    if (!conn->connected())
                    {
                        return;
                    }
                    auto &ps = (*partsState)[idx];
                    ps.session = makePoolShared<TransferSession>(
                        TransferSession::Role::kClient, conn, std::string(), authToken_, idleTimeoutSec_);
                    if (progressCallback_)
                    {
                        ps.session->setProgressCallback(progressCallback_);
                    }
                    ps.session->startIdleGuard();
                    conn->setContext(ps.session);
                    ps.session->setDoneCallback(
                        [remain, failed, done, this](bool okPart, TransferErrorCode code, const std::string &m) {
                            if (!okPart)
                            {
                                failed->store(true);
                            }
                            if (remain->fetch_sub(1) == 1)
                            {
                                const bool allOk = !failed->load();
                                if (done)
                                {
                                    done(allOk,
                                         allOk ? TransferErrorCode::kOk : code,
                                         allOk ? "download parallel done" : m);
                                }
                                notifyDone(allOk,
                                           allOk ? TransferErrorCode::kOk : code,
                                           allOk ? "download parallel done" : m);
                            }
                        });
                    ps.session->setAuthToken(authToken_);
                    ps.session->startDownload(remoteRelPath, localPath, offset, length);
                });
            (*partsState)[idx].client->setMessageCallback(
                [](const TcpConnectionPtr &c, Buffer *buf, Timestamp) {
                    auto s = c->getContext<TransferSession>();
                    if (s)
                    {
                        s->onMessage(buf);
                    }
                });
            (*partsState)[idx].client->connect();
        }
    });
}

bool FileTransferClient::waitDone(int timeoutMs, bool *okOut, std::string *messageOut)
{
    std::unique_lock<std::mutex> lock(doneMutex_);
    const bool got = doneCv_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this]() { return doneReady_; });
    if (okOut)
    {
        *okOut = lastOk_;
    }
    if (messageOut)
    {
        *messageOut = lastMessage_;
    }
    return got;
}

} // namespace transport
