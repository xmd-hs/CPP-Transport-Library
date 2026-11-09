#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "InetAddress.h"
#include "KademliaNode.h"
#include "PeerSession.h"
#include "PieceStore.h"
#include "TcpClient.h"
#include "TcpServer.h"
#include "TorrentMeta.h"

namespace transport
{
namespace p2p
{

struct SwarmProgress
{
    uint32_t have = 0;
    uint32_t total = 0;
    size_t peers = 0;
};

class TorrentSession : public std::enable_shared_from_this<TorrentSession>
{
public:
    using ProgressCallback = std::function<void(const SwarmProgress &)>;
    using DoneCallback = std::function<void(bool ok, const std::string &msg)>;

    TorrentSession(EventLoop *loop,
                   TorrentMeta meta,
                   std::string filePath,
                   uint16_t udpPort,
                   uint16_t tcpPort,
                   bool seedMode);

    void setBootstrap(const std::vector<InetAddress> &seeds) { bootstrap_ = seeds; }
    void setAdvertiseIPv4(uint32_t be) { advertiseIPv4_ = be; }
    void setProgressCallback(ProgressCallback cb) { progressCb_ = std::move(cb); }
    void setDoneCallback(DoneCallback cb) { doneCb_ = std::move(cb); }

    void start();
    void stop();

    std::shared_ptr<KademliaNode> dht() const { return dht_; }
    PieceStore &store() { return store_; }

private:
    void onPeerConnected(const TcpConnectionPtr &conn, bool outbound);
    void onPeerMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp);
    void addOutboundPeer(const PeerContact &c);
    void tryConnectKnownPeers();
    void schedulePieces();
    void onPieceReceived(uint32_t index, const void *data, size_t len);
    void refreshPeers();
    void reportProgress();
    void finish(bool ok, const std::string &msg);

    EventLoop *loop_;
    TorrentMeta meta_;
    std::string filePath_;
    uint16_t udpPort_;
    uint16_t tcpPort_;
    bool seedMode_;
    uint32_t advertiseIPv4_ = 0;
    std::vector<InetAddress> bootstrap_;

    std::shared_ptr<KademliaNode> dht_;
    PieceStore store_;
    std::unique_ptr<TcpServer> server_;
    std::vector<std::unique_ptr<TcpClient>> clients_;
    std::mutex peersMu_;
    std::vector<std::shared_ptr<PeerSession>> peers_;
    std::unordered_set<std::string> pendingDial_;
    std::unordered_set<uint32_t> requested_;
    std::unordered_set<uint32_t> missing_;

    ProgressCallback progressCb_;
    DoneCallback doneCb_;
    std::atomic_bool finished_{false};
    TimerId peerRefreshTimer_;
    TimerId scheduleTimer_;
};

} // namespace p2p
} // namespace transport
