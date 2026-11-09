#include "TorrentSession.h"

#include <algorithm>

#include "EventLoop.h"
#include "Logger.h"

namespace transport
{
namespace p2p
{

namespace
{
constexpr size_t kMaxInflightPerPeer = 24;
}

TorrentSession::TorrentSession(EventLoop *loop,
                               TorrentMeta meta,
                               std::string filePath,
                               uint16_t udpPort,
                               uint16_t tcpPort,
                               bool seedMode)
    : loop_(loop)
    , meta_(std::move(meta))
    , filePath_(std::move(filePath))
    , udpPort_(udpPort)
    , tcpPort_(tcpPort)
    , seedMode_(seedMode)
{
}

void TorrentSession::start()
{
    if (!store_.open(meta_, filePath_, seedMode_))
    {
        finish(false, "open piece store failed");
        return;
    }

    if (advertiseIPv4_ == 0)
    {
        advertiseIPv4_ = detectAdvertiseIPv4();
    }
    dht_ = std::make_shared<KademliaNode>(loop_, InetAddress(udpPort_, "0.0.0.0"), tcpPort_);
    dht_->setAdvertiseIPv4(advertiseIPv4_);
    dht_->start();
    if (!bootstrap_.empty())
    {
        dht_->bootstrap(bootstrap_);
    }
    dht_->announce(meta_.infoHash);

    server_.reset(new TcpServer(loop_, InetAddress(tcpPort_, "0.0.0.0"), "P2PPeerServer"));
    server_->setConnectionCallback([this](const TcpConnectionPtr &conn) {
        if (conn->connected())
        {
            onPeerConnected(conn, false);
        }
    });
    server_->setMessageCallback(
        [this](const TcpConnectionPtr &c, Buffer *b, Timestamp t) { onPeerMessage(c, b, t); });
    server_->start();

    peerRefreshTimer_ = loop_->runEvery(8.0, [self = shared_from_this()]() {
        self->refreshPeers();
        self->schedulePieces();
    });
    // 主路径靠 ready/have/piece 回调；定时器仅兜底。
    scheduleTimer_ = loop_->runEvery(1.0, [self = shared_from_this()]() { self->schedulePieces(); });

    loop_->runAfter(0.3, [self = shared_from_this()]() { self->refreshPeers(); });
    loop_->runAfter(1.0, [self = shared_from_this()]() { self->refreshPeers(); });

    missing_.clear();
    for (uint32_t i = 0; i < meta_.pieceCount(); ++i)
    {
        if (!store_.hasPiece(i))
        {
            missing_.insert(i);
        }
    }
    reportProgress();
    if (store_.complete())
    {
        LOG_INFO("TorrentSession already complete, seeding\n");
    }
}

void TorrentSession::stop()
{
    loop_->cancel(peerRefreshTimer_);
    loop_->cancel(scheduleTimer_);
    if (dht_)
    {
        dht_->stop();
    }
}

void TorrentSession::tryConnectKnownPeers()
{
    for (const auto &c : dht_->knownNodes())
    {
        if (c.tcpPort > 0)
        {
            addOutboundPeer(c);
        }
    }
}

void TorrentSession::refreshPeers()
{
    tryConnectKnownPeers();
    dht_->getPeers(meta_.infoHash, [self = shared_from_this()](const std::vector<PeerContact> &peers) {
        for (const auto &c : peers)
        {
            self->addOutboundPeer(c);
        }
        self->tryConnectKnownPeers();
    });
    dht_->announce(meta_.infoHash);
}

void TorrentSession::addOutboundPeer(const PeerContact &c)
{
    if (idEqual(c.id, dht_->selfId()) || c.tcpPort == 0)
    {
        return;
    }
    const InetAddress addr = c.toTcpAddr();
    {
        std::lock_guard<std::mutex> lock(peersMu_);
        for (const auto &p : peers_)
        {
            if (p->connection() &&
                p->connection()->peerAddress().toPort() == c.tcpPort &&
                p->connection()->peerAddress().toIp() == addr.toIp())
            {
                return;
            }
        }
        for (const auto &cli : clients_)
        {
            auto conn = cli->connection();
            if (conn &&
                conn->peerAddress().toPort() == c.tcpPort &&
                conn->peerAddress().toIp() == addr.toIp())
            {
                return;
            }
        }
        if (pendingDial_.count(addr.toIpPort()))
        {
            return;
        }
        pendingDial_.insert(addr.toIpPort());
    }

    LOG_INFO("dial peer %s\n", addr.toIpPort().c_str());
    auto client = std::unique_ptr<TcpClient>(
        new TcpClient(loop_, addr, "P2PPeerClient"));
    TcpClient *raw = client.get();
    raw->enableRetry();
    raw->setConnectionCallback([this, addr](const TcpConnectionPtr &conn) {
        if (conn->connected())
        {
            {
                std::lock_guard<std::mutex> lock(peersMu_);
                pendingDial_.erase(addr.toIpPort());
            }
            onPeerConnected(conn, true);
        }
    });
    raw->setMessageCallback(
        [this](const TcpConnectionPtr &c, Buffer *b, Timestamp t) { onPeerMessage(c, b, t); });
    raw->connect();
    clients_.push_back(std::move(client));
}

void TorrentSession::onPeerConnected(const TcpConnectionPtr &conn, bool outbound)
{
    auto session = std::make_shared<PeerSession>(
        conn, &store_, meta_.infoHash, dht_->selfId(), outbound);
    session->setPieceHandler(
        [self = shared_from_this()](uint32_t idx, const void *data, size_t len) {
            self->onPieceReceived(idx, data, len);
        });
    session->setHaveHandler([self = shared_from_this()](const std::shared_ptr<PeerSession> &, uint32_t) {
        self->schedulePieces();
    });
    session->setReadyHandler([self = shared_from_this()](const std::shared_ptr<PeerSession> &) {
        self->schedulePieces();
    });
    conn->setContext(session);
    {
        std::lock_guard<std::mutex> lock(peersMu_);
        peers_.push_back(session);
    }
    session->start();
    LOG_INFO("peer %s %s\n", outbound ? "out" : "in", conn->peerAddress().toIpPort().c_str());
    reportProgress();
}

void TorrentSession::onPeerMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    auto s = conn->getContext<PeerSession>();
    if (s)
    {
        s->onMessage(buf);
    }
}

void TorrentSession::onPieceReceived(uint32_t index, const void *data, size_t len)
{
    requested_.erase(index);
    if (store_.hasPiece(index))
    {
        return;
    }
    if (!store_.writePiece(index, data, len))
    {
        LOG_ERROR("piece %u hash/write failed\n", index);
        return;
    }
    missing_.erase(index);
    const uint32_t have = store_.haveCount();
    if (have == meta_.pieceCount() || (have % 8) == 0)
    {
        LOG_INFO("got piece %u have=%u/%u\n", index, have, meta_.pieceCount());
    }

    std::vector<std::shared_ptr<PeerSession>> snap;
    {
        std::lock_guard<std::mutex> lock(peersMu_);
        snap = peers_;
    }
    for (auto &p : snap)
    {
        // endgame 下取消其它 peer 对同片的在途请求，省带宽。
        if (p->hasInflight(index))
        {
            p->cancelPiece(index);
        }
        p->broadcastHave(index);
    }
    reportProgress();
    if (store_.complete() || missing_.empty())
    {
        finish(true, "download complete");
    }
    else
    {
        schedulePieces();
    }
}

void TorrentSession::schedulePieces()
{
    if (store_.complete() || missing_.empty())
    {
        return;
    }

    std::vector<std::shared_ptr<PeerSession>> snap;
    {
        std::lock_guard<std::mutex> lock(peersMu_);
        snap = peers_;
    }
    if (snap.empty())
    {
        return;
    }

    const bool endgame = missing_.size() <= snap.size() * 2;

    std::vector<uint32_t> candidates;
    candidates.reserve(missing_.size());
    std::vector<int> rarity(meta_.pieceCount(), 0);
    for (uint32_t idx : missing_)
    {
        if (!endgame && requested_.count(idx))
        {
            continue;
        }
        candidates.push_back(idx);
        for (auto &p : snap)
        {
            if (p->handshaked() && p->peerHas(idx))
            {
                ++rarity[idx];
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](uint32_t a, uint32_t b) {
        if (rarity[a] == 0 && rarity[b] == 0)
            return a < b;
        if (rarity[a] == 0)
            return false;
        if (rarity[b] == 0)
            return true;
        if (rarity[a] != rarity[b])
            return rarity[a] < rarity[b];
        return a < b;
    });

    for (uint32_t idx : candidates)
    {
        for (auto &p : snap)
        {
            if (!p->handshaked() || !p->peerHas(idx) || p->hasInflight(idx))
            {
                continue;
            }
            if (p->inflight() >= kMaxInflightPerPeer)
            {
                continue;
            }
            p->requestPiece(idx);
            requested_.insert(idx);
            if (!endgame)
            {
                break;
            }
        }
    }
}

void TorrentSession::reportProgress()
{
    if (!progressCb_)
    {
        return;
    }
    SwarmProgress p;
    p.have = store_.haveCount();
    p.total = meta_.pieceCount();
    std::lock_guard<std::mutex> lock(peersMu_);
    p.peers = peers_.size();
    progressCb_(p);
}

void TorrentSession::finish(bool ok, const std::string &msg)
{
    if (finished_.exchange(true))
    {
        return;
    }
    if (doneCb_)
    {
        doneCb_(ok, msg);
    }
}

} // namespace p2p
} // namespace transport
