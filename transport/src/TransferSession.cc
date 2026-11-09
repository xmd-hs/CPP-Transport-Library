#include "TransferSession.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "Codec.h"
#include "EventLoop.h"
#include "Logger.h"
#include "Protocol.h"
#include "TcpConnection.h"

namespace transport
{

TransferSession::TransferSession(Role role,
                                 const TcpConnectionPtr &conn,
                                 std::string rootDir,
                                 std::string authToken,
                                 double idleTimeoutSec)
    : role_(role)
    , conn_(conn)
    , rootDir_(std::move(rootDir))
    , authToken_(std::move(authToken))
    , idleTimeoutSec_(idleTimeoutSec)
    , state_(State::kIdle)
    , fileFd_(-1)
    , offset_(0)
    , fileSize_(0)
    , sessionTotal_(0)
    , transferred_(0)
    , upload_(false)
    , finished_(false)
{
    // 不在构造函数里 armIdleTimer：此时 shared_from_this 尚未可用。
}

TransferSession::~TransferSession()
{
    cancelIdleTimer();
    closeFile();
}

void TransferSession::closeFile()
{
    if (fileFd_ >= 0)
    {
        ::close(fileFd_);
        fileFd_ = -1;
    }
}

void TransferSession::startIdleGuard()
{
    armIdleTimer();
}

void TransferSession::armIdleTimer()
{
    if (!conn_ || idleTimeoutSec_ <= 0)
    {
        return;
    }
    cancelIdleTimer();
    lastActivity_ = std::chrono::steady_clock::now();
    idleTimer_ = conn_->getLoop()->runAfter(idleTimeoutSec_, [self = shared_from_this()]() {
        self->onIdleTimeout();
    });
}

void TransferSession::cancelIdleTimer()
{
    if (conn_)
    {
        conn_->getLoop()->cancel(idleTimer_);
    }
    idleTimer_ = TimerId();
}

void TransferSession::touch()
{
    // Activity only updates a timestamp; avoid timer allocation/tree updates per packet.
    lastActivity_ = std::chrono::steady_clock::now();
}

void TransferSession::onIdleTimeout()
{
    // A long file send may make progress without a write-complete callback.
    // Check actual sendfile activity rather than treating that as idle time.
    if (conn_)
    {
        lastActivity_ = std::max(lastActivity_, conn_->lastFileWriteTime());
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - lastActivity_).count();
    if (elapsed < idleTimeoutSec_)
    {
        idleTimer_ = conn_->getLoop()->runAfter(idleTimeoutSec_ - elapsed,
            [self = shared_from_this()]() { self->onIdleTimeout(); });
        return;
    }
    if (state_ == State::kIdle)
    {
        LOG_INFO("TransferSession idle timeout, closing %s\n",
                 conn_ ? conn_->name().c_str() : "");
        if (conn_)
        {
            conn_->shutdown();
        }
        return;
    }
    finish(false, TransferErrorCode::kIOError, "idle timeout during transfer");
    if (conn_)
    {
        conn_->shutdown();
    }
}

void TransferSession::reportProgress()
{
    if (!progressCallback_)
    {
        return;
    }
    TransferProgress p;
    p.path = path_;
    p.transferred = transferred_;
    p.total = sessionTotal_;
    p.upload = upload_;
    progressCallback_(p);
}

void TransferSession::resetToIdle()
{
    closeFile();
    state_ = State::kIdle;
    finished_ = false;
    path_.clear();
    offset_ = 0;
    fileSize_ = 0;
    sessionTotal_ = 0;
    transferred_ = 0;
    statCallback_ = nullptr;
    if (conn_)
    {
        conn_->setWriteCompleteCallback(WriteCompleteCallback());
    }
    armIdleTimer();
}

void TransferSession::finish(bool ok, TransferErrorCode code, const std::string &message)
{
    if (finished_.exchange(true))
    {
        return;
    }
    cancelIdleTimer();
    closeFile();
    if (conn_)
    {
        conn_->setWriteCompleteCallback(WriteCompleteCallback());
    }

    const auto cb = doneCallback_;
    // 服务端与客户端成功后都回到 Idle，支持同连接多次传输。
    resetToIdle();
    if (!ok && role_ == Role::kClient)
    {
        state_ = State::kFinished;
        finished_ = true;
    }
    if (cb)
    {
        cb(ok, code, message);
    }
}

void TransferSession::sendError(TransferErrorCode code, const std::string &message)
{
    if (conn_ && conn_->connected())
    {
        const std::string frame = Codec::encodeFrame(
            MessageType::kError, "", message.data(), message.size(), static_cast<uint64_t>(code), 0);
        conn_->send(frame);
    }
    finish(false, code, message);
}

bool TransferSession::checkAuth(const std::string &token) const
{
    if (authToken_.empty())
    {
        return true;
    }
    return token == authToken_;
}

bool TransferSession::resolveServerPath(const std::string &rel, std::string *absOut) const
{
    return protocol::resolveUnderRoot(rootDir_, rel, absOut);
}

void TransferSession::onClosed()
{
    cancelIdleTimer();
    if (state_ != State::kIdle && state_ != State::kFinished)
    {
        finish(false, TransferErrorCode::kIOError, "connection closed");
    }
}

void TransferSession::onMessage(Buffer *buf)
{
    touch();
    if (state_ == State::kServerReceivingUpload)
    {
        consumeUploadData(buf);
        return;
    }
    if (state_ == State::kClientReceivingDownload)
    {
        consumeDownloadData(buf);
        return;
    }

    while (true)
    {
        if (state_ == State::kServerReceivingUpload)
        {
            consumeUploadData(buf);
            return;
        }
        if (state_ == State::kClientReceivingDownload)
        {
            consumeDownloadData(buf);
            return;
        }

        DecodedFrame frame;
        const DecodeStatus st = Codec::tryDecode(buf, &frame);
        if (st == DecodeStatus::kNeedMore)
        {
            return;
        }
        if (st == DecodeStatus::kError)
        {
            sendError(TransferErrorCode::kBadChecksum, "bad frame/checksum");
            if (conn_)
            {
                conn_->shutdown();
            }
            return;
        }

        if (role_ == Role::kServer)
        {
            handleServerFrame(frame);
        }
        else
        {
            handleClientFrame(frame);
        }

        if (state_ == State::kFinished)
        {
            return;
        }
    }
}

void TransferSession::handleServerFrame(const DecodedFrame &frame)
{
    if (state_ != State::kIdle)
    {
        sendError(TransferErrorCode::kBusy, "transfer in progress");
        return;
    }
    if (!checkAuth(frame.token))
    {
        sendError(TransferErrorCode::kUnauthorized, "unauthorized");
        return;
    }

    const auto type = static_cast<MessageType>(frame.header.type);
    if (type == MessageType::kUploadReq)
    {
        beginServerUpload(frame.path, frame.header.offset, frame.header.fileSize);
    }
    else if (type == MessageType::kDownloadReq)
    {
        beginServerDownload(frame.path, frame.header.offset, frame.header.bodyLen);
    }
    else if (type == MessageType::kStatReq)
    {
        beginServerStat(frame.path);
    }
    else if (type == MessageType::kError)
    {
        finish(false, static_cast<TransferErrorCode>(frame.header.offset), frame.body);
    }
    else
    {
        sendError(TransferErrorCode::kProtocol, "unexpected message");
    }
}

void TransferSession::handleClientFrame(const DecodedFrame &frame)
{
    const auto type = static_cast<MessageType>(frame.header.type);
    if (type == MessageType::kError)
    {
        finish(false, static_cast<TransferErrorCode>(frame.header.offset), frame.body);
        return;
    }

    if (state_ == State::kClientWaitDownloadResp && type == MessageType::kDownloadResp)
    {
        sessionTotal_ = frame.header.bodyLen;
        fileSize_ = frame.header.fileSize;
        offset_ = frame.header.offset;
        transferred_ = 0;
        upload_ = false;
        state_ = State::kClientReceivingDownload;
        reportProgress();
        return;
    }

    if (state_ == State::kClientSendingUpload && type == MessageType::kUploadDone)
    {
        finish(true, TransferErrorCode::kOk, "upload done");
        return;
    }

    if (state_ == State::kClientWaitStatResp && type == MessageType::kStatResp)
    {
        const auto cb = statCallback_;
        resetToIdle();
        if (cb)
        {
            cb(true, frame.header.fileSize, std::string());
        }
        return;
    }

    sendError(TransferErrorCode::kProtocol, "unexpected message");
}

void TransferSession::beginServerUpload(const std::string &relPath,
                                        uint64_t offset,
                                        uint64_t fileSize)
{
    std::string absPath;
    if (!resolveServerPath(relPath, &absPath))
    {
        sendError(TransferErrorCode::kBadPath, "invalid path");
        return;
    }
    if (fileSize < offset)
    {
        sendError(TransferErrorCode::kProtocol, "offset > fileSize");
        return;
    }

    const auto slash = absPath.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
    {
        const std::string dir = absPath.substr(0, slash);
        std::string cur;
        for (size_t i = 0; i < dir.size(); ++i)
        {
            cur.push_back(dir[i]);
            if (dir[i] == '/' || i + 1 == dir.size())
            {
                if (cur != "/" && !cur.empty())
                {
                    ::mkdir(cur.c_str(), 0755);
                }
            }
        }
    }

    const int fd = ::open(absPath.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0)
    {
        sendError(TransferErrorCode::kIOError, "open for upload failed");
        return;
    }
    if (::ftruncate(fd, static_cast<off_t>(fileSize)) < 0 ||
        ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
    {
        ::close(fd);
        sendError(TransferErrorCode::kIOError, "prepare upload file failed");
        return;
    }

    fileFd_ = fd;
    path_ = relPath;
    offset_ = offset;
    fileSize_ = fileSize;
    sessionTotal_ = fileSize - offset;
    transferred_ = 0;
    upload_ = true;
    finished_ = false;
    state_ = State::kServerReceivingUpload;
    reportProgress();
}

void TransferSession::beginServerDownload(const std::string &relPath,
                                          uint64_t offset,
                                          uint64_t length)
{
    std::string absPath;
    if (!resolveServerPath(relPath, &absPath))
    {
        sendError(TransferErrorCode::kBadPath, "invalid path");
        return;
    }

    const int fd = ::open(absPath.c_str(), O_RDONLY);
    if (fd < 0)
    {
        sendError(TransferErrorCode::kNotFound, "file not found");
        return;
    }

    struct stat st;
    if (::fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
    {
        ::close(fd);
        sendError(TransferErrorCode::kIOError, "stat failed");
        return;
    }

    const uint64_t total = static_cast<uint64_t>(st.st_size);
    if (offset > total)
    {
        ::close(fd);
        sendError(TransferErrorCode::kProtocol, "offset past EOF");
        return;
    }

    uint64_t toSend = total - offset;
    if (length > 0)
    {
        toSend = std::min(toSend, length);
    }

    fileFd_ = fd;
    path_ = relPath;
    offset_ = offset;
    fileSize_ = total;
    sessionTotal_ = toSend;
    transferred_ = 0;
    upload_ = false;
    finished_ = false;
    state_ = State::kServerSendingDownload;

    const std::string resp = Codec::encodeFrame(
        MessageType::kDownloadResp, "", nullptr, toSend, offset, total, kFlagFinal);
    conn_->send(resp);
    conn_->setWriteCompleteCallback(
        [self = shared_from_this()](const TcpConnectionPtr &) { self->onWriteComplete(); });
    reportProgress();
    pumpServerDownload();
}

void TransferSession::beginServerStat(const std::string &relPath)
{
    std::string absPath;
    if (!resolveServerPath(relPath, &absPath))
    {
        sendError(TransferErrorCode::kBadPath, "invalid path");
        return;
    }
    struct stat st;
    if (::stat(absPath.c_str(), &st) < 0 || !S_ISREG(st.st_mode))
    {
        sendError(TransferErrorCode::kNotFound, "file not found");
        return;
    }
    const std::string resp = Codec::encodeFrame(
        MessageType::kStatResp,
        protocol::encodeExtra(std::string(), relPath),
        nullptr,
        0,
        0,
        static_cast<uint64_t>(st.st_size));
    conn_->send(resp);
}

void TransferSession::consumeUploadData(Buffer *buf)
{
    while (transferred_ < sessionTotal_ && buf->readableBytes() > 0)
    {
        const size_t n = static_cast<size_t>(
            std::min<uint64_t>(sessionTotal_ - transferred_, buf->readableBytes()));
        const ssize_t w = ::write(fileFd_, buf->peek(), n);
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            sendError(TransferErrorCode::kIOError, "write failed");
            return;
        }
        buf->retrieve(static_cast<size_t>(w));
        transferred_ += static_cast<uint64_t>(w);
        reportProgress();
    }

    if (transferred_ >= sessionTotal_)
    {
        ::fsync(fileFd_);
        closeFile();
        const std::string done = Codec::encodeFrame(
            MessageType::kUploadDone, path_, nullptr, 0, offset_, fileSize_, kFlagFinal);
        conn_->send(done);
        finish(true, TransferErrorCode::kOk, "upload done");
    }
}

void TransferSession::consumeDownloadData(Buffer *buf)
{
    while (transferred_ < sessionTotal_ && buf->readableBytes() > 0)
    {
        const size_t n = static_cast<size_t>(
            std::min<uint64_t>(sessionTotal_ - transferred_, buf->readableBytes()));
        const ssize_t w = ::write(fileFd_, buf->peek(), n);
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            finish(false, TransferErrorCode::kIOError, "local write failed");
            return;
        }
        buf->retrieve(static_cast<size_t>(w));
        transferred_ += static_cast<uint64_t>(w);
        reportProgress();
    }

    if (transferred_ >= sessionTotal_)
    {
        ::fsync(fileFd_);
        finish(true, TransferErrorCode::kOk, "download done");
    }
}

void TransferSession::pumpServerDownload()
{
    if (state_ != State::kServerSendingDownload || !conn_ || !conn_->connected())
    {
        return;
    }
    if (transferred_ >= sessionTotal_)
    {
        if (conn_->outputBufferBytes() == 0 && !conn_->isWriting())
        {
            finish(true, TransferErrorCode::kOk, "download done");
        }
        return;
    }

    const uint64_t left = sessionTotal_ - transferred_;
    const off_t off = static_cast<off_t>(offset_ + transferred_);
    conn_->sendFile(fileFd_, off, static_cast<size_t>(left));
    transferred_ = sessionTotal_;
    reportProgress();
}

void TransferSession::onWriteComplete()
{
    touch();
    if (state_ == State::kServerSendingDownload)
    {
        if (transferred_ >= sessionTotal_ &&
            conn_->outputBufferBytes() == 0 && !conn_->isWriting())
        {
            finish(true, TransferErrorCode::kOk, "download done");
        }
        else
        {
            pumpServerDownload();
        }
        return;
    }
    if (state_ == State::kClientSendingUpload)
    {
        pumpClientUpload();
    }
}

void TransferSession::startUpload(const std::string &localPath,
                                  const std::string &remoteRelPath,
                                  uint64_t offset)
{
    if (role_ != Role::kClient || state_ != State::kIdle)
    {
        finish(false, TransferErrorCode::kBusy, "cannot start upload");
        return;
    }
    std::string normalized;
    if (!protocol::normalizeRelPath(remoteRelPath, &normalized))
    {
        finish(false, TransferErrorCode::kBadPath, "invalid remote path");
        return;
    }

    const int fd = ::open(localPath.c_str(), O_RDONLY);
    if (fd < 0)
    {
        finish(false, TransferErrorCode::kNotFound, "local file not found");
        return;
    }
    struct stat st;
    if (::fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        offset > static_cast<uint64_t>(st.st_size) ||
        ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
    {
        ::close(fd);
        finish(false, TransferErrorCode::kIOError, "prepare local upload failed");
        return;
    }

    fileFd_ = fd;
    path_ = normalized;
    offset_ = offset;
    fileSize_ = static_cast<uint64_t>(st.st_size);
    sessionTotal_ = fileSize_ - offset;
    transferred_ = 0;
    upload_ = true;
    finished_ = false;
    state_ = State::kClientSendingUpload;

    const std::string extra = protocol::encodeExtra(authToken_, normalized);
    const std::string req =
        Codec::encodeFrame(MessageType::kUploadReq, extra, nullptr, 0, offset, fileSize_);
    conn_->send(req);
    conn_->setWriteCompleteCallback(
        [self = shared_from_this()](const TcpConnectionPtr &) { self->onWriteComplete(); });
    reportProgress();
    pumpClientUpload();
}

void TransferSession::pumpClientUpload()
{
    if (state_ != State::kClientSendingUpload || !conn_ || !conn_->connected() ||
        conn_->isSendingFile() || transferred_ >= sessionTotal_)
    {
        return;
    }
    // Enqueue one file range, avoiding a completion callback and descriptor
    // duplication per MiB. TcpConnection bounds each actual sendfile syscall.
    const size_t count = static_cast<size_t>(sessionTotal_ - transferred_);
    conn_->sendFile(fileFd_, static_cast<off_t>(offset_ + transferred_), count);
    transferred_ += count;
    reportProgress();
}

void TransferSession::startDownload(const std::string &remoteRelPath,
                                    const std::string &localPath,
                                    uint64_t offset,
                                    uint64_t length)
{
    if (role_ != Role::kClient || state_ != State::kIdle)
    {
        finish(false, TransferErrorCode::kBusy, "cannot start download");
        return;
    }
    std::string normalized;
    if (!protocol::normalizeRelPath(remoteRelPath, &normalized))
    {
        finish(false, TransferErrorCode::kBadPath, "invalid remote path");
        return;
    }

    // A ranged first part shares a pre-sized file with other connections; it
    // must not truncate data those connections may already have written.
    const int flags = O_RDWR | O_CREAT | (offset == 0 && length == 0 ? O_TRUNC : 0);
    const int fd = ::open(localPath.c_str(), flags, 0644);
    if (fd < 0 || (offset > 0 && ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0))
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
        finish(false, TransferErrorCode::kIOError, "open local failed");
        return;
    }

    fileFd_ = fd;
    path_ = normalized;
    offset_ = offset;
    fileSize_ = 0;
    sessionTotal_ = 0;
    transferred_ = 0;
    upload_ = false;
    finished_ = false;
    state_ = State::kClientWaitDownloadResp;

    const std::string extra = protocol::encodeExtra(authToken_, normalized);
    const std::string req =
        Codec::encodeFrame(MessageType::kDownloadReq, extra, nullptr, length, offset, 0);
    conn_->send(req);
    reportProgress();
}

void TransferSession::startStat(const std::string &remoteRelPath,
                                std::function<void(bool, uint64_t, const std::string &)> cb)
{
    if (role_ != Role::kClient || state_ != State::kIdle)
    {
        if (cb)
        {
            cb(false, 0, "busy");
        }
        return;
    }
    std::string normalized;
    if (!protocol::normalizeRelPath(remoteRelPath, &normalized))
    {
        if (cb)
        {
            cb(false, 0, "bad path");
        }
        return;
    }
    finished_ = false;
    state_ = State::kClientWaitStatResp;
    statCallback_ = std::move(cb);
    const std::string extra = protocol::encodeExtra(authToken_, normalized);
    const std::string req = Codec::encodeFrame(MessageType::kStatReq, extra, nullptr, 0, 0, 0);
    conn_->send(req);
}

} // namespace transport
