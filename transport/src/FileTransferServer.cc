#include "FileTransferServer.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "Logger.h"

namespace transport
{
namespace
{

void ensureDir(const std::string &dir)
{
    if (dir.empty())
    {
        return;
    }
    struct stat st;
    if (::stat(dir.c_str(), &st) == 0)
    {
        return;
    }
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i)
    {
        cur.push_back(dir[i]);
        if (dir[i] == '/' || i + 1 == dir.size())
        {
            if (cur == "/" || cur.empty())
            {
                continue;
            }
            ::mkdir(cur.c_str(), 0755);
        }
    }
}

} // namespace

FileTransferServer::FileTransferServer(EventLoop *loop,
                                       const InetAddress &listenAddr,
                                       std::string rootDir,
                                       const std::string &nameArg)
    : server_(loop, listenAddr, nameArg)
    , rootDir_(std::move(rootDir))
    , idleTimeoutSec_(60.0)
{
    ensureDir(rootDir_);
    server_.setConnectionCallback(
        std::bind(&FileTransferServer::onConnection, this, std::placeholders::_1));
    server_.setMessageCallback(
        std::bind(&FileTransferServer::onMessage,
                  this,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  std::placeholders::_3));
}

void FileTransferServer::start()
{
    LOG_INFO("FileTransferServer start root=%s idle=%.1fs auth=%s\n",
             rootDir_.c_str(),
             idleTimeoutSec_,
             authToken_.empty() ? "off" : "on");
    server_.start();
}

void FileTransferServer::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        auto session = makePoolShared<TransferSession>(
            TransferSession::Role::kServer, conn, rootDir_, authToken_, idleTimeoutSec_);
        if (progressCallback_)
        {
            session->setProgressCallback(progressCallback_);
        }
        if (doneCallback_)
        {
            session->setDoneCallback(doneCallback_);
        }
        session->startIdleGuard();
        conn->setContext(session);
    }
    else
    {
        auto session = conn->getContext<TransferSession>();
        if (session)
        {
            session->onClosed();
        }
        conn->setContext(std::shared_ptr<void>());
    }
}

void FileTransferServer::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    auto session = conn->getContext<TransferSession>();
    if (session)
    {
        session->onMessage(buf);
    }
}

} // namespace transport
