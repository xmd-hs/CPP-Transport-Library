#include "KademliaNode.h"

#include <arpa/inet.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <chrono>

#include "EventLoop.h"
#include "Logger.h"
#include "Sha1.h"

namespace transport
{
namespace p2p
{
namespace
{

void putU16(std::string *o, uint16_t v)
{
    const uint16_t n = htons(v);
    o->append(reinterpret_cast<const char *>(&n), 2);
}
void putU32(std::string *o, uint32_t v)
{
    const uint32_t n = htonl(v);
    o->append(reinterpret_cast<const char *>(&n), 4);
}
bool getU16(const char *&p, const char *e, uint16_t *v)
{
    if (e - p < 2)
        return false;
    uint16_t n;
    std::memcpy(&n, p, 2);
    *v = ntohs(n);
    p += 2;
    return true;
}
bool getU32(const char *&p, const char *e, uint32_t *v)
{
    if (e - p < 4)
        return false;
    uint32_t n;
    std::memcpy(&n, p, 4);
    *v = ntohl(n);
    p += 4;
    return true;
}
bool getId(const char *&p, const char *e, NodeId *id)
{
    if (e - p < 20)
        return false;
    std::memcpy(id->bytes, p, 20);
    p += 20;
    return true;
}
void putId(std::string *o, const NodeId &id)
{
    o->append(reinterpret_cast<const char *>(id.bytes), 20);
}
void putContact(std::string *o, const PeerContact &c)
{
    putId(o, c.id);
    o->append(reinterpret_cast<const char *>(&c.ipv4), 4);
    putU16(o, c.udpPort);
    putU16(o, c.tcpPort);
}
bool getContact(const char *&p, const char *e, PeerContact *c)
{
    if (!getId(p, e, &c->id) || e - p < 8)
        return false;
    std::memcpy(&c->ipv4, p, 4);
    p += 4;
    return getU16(p, e, &c->udpPort) && getU16(p, e, &c->tcpPort);
}

} // namespace

std::string KademliaNode::idHex(const NodeId &id)
{
    return Sha1::toHex(id);
}

KademliaNode::KademliaNode(EventLoop *loop,
                           const InetAddress &udpBind,
                           uint16_t advertiseTcpPort,
                           NodeId selfId)
    : loop_(loop)
    , selfId_(idEqual(selfId, NodeId{}) ? randomNodeId() : selfId)
    , tcpPort_(advertiseTcpPort)
    , udp_(new UdpSocket(loop, udpBind))
    , table_(selfId_)
    , nextTxid_(1)
    , tokenSecret_(static_cast<uint32_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()))
{
    udp_->setMessageCallback([this](const char *d, size_t n, const InetAddress &a) {
        onUdpMessage(d, n, a);
    });
}

PeerContact KademliaNode::selfContact() const
{
    PeerContact c;
    c.id = selfId_;
    if (advertiseIPv4_ != 0)
    {
        c.ipv4 = advertiseIPv4_;
    }
    else
    {
        c.ipv4 = udp_->localAddress().getSockAddr()->sin_addr.s_addr;
        if (c.ipv4 == 0)
        {
            c.ipv4 = htonl(INADDR_LOOPBACK);
        }
    }
    c.udpPort = udp_->localAddress().toPort();
    c.tcpPort = tcpPort_;
    return c;
}

void KademliaNode::start()
{
    udp_->start();
    refreshTimer_ = loop_->runEvery(20.0, [self = shared_from_this()]() {
        for (const auto &c : self->table_.closest(self->selfId_, kBucketSize))
        {
            self->sendPacket(kPing, self->nextTxid(), c.toUdpAddr(), std::string());
        }
        if (self->announcing_)
        {
            self->announce(self->announceHash_);
        }
    });
    LOG_INFO("Kademlia start id=%s udp=%u tcp=%u\n",
             idHex(selfId_).c_str(),
             udpPort(),
             tcpPort_);
}

void KademliaNode::stop()
{
    announcing_ = false;
    loop_->cancel(refreshTimer_);
    loop_->cancel(announceTimer_);
    for (auto &kv : pending_)
    {
        loop_->cancel(kv.second.timer);
    }
    pending_.clear();
    udp_->stop();
}

uint32_t KademliaNode::nextTxid()
{
    return nextTxid_++;
}

uint32_t KademliaNode::makeToken(const InetAddress &requester) const
{
    const uint32_t bucket = static_cast<uint32_t>(::time(nullptr) / 600);
    uint8_t buf[12];
    std::memcpy(buf, &tokenSecret_, 4);
    std::memcpy(buf + 4, &requester.getSockAddr()->sin_addr.s_addr, 4);
    std::memcpy(buf + 8, &bucket, 4);
    const auto d = Sha1::hash(buf, 12);
    uint32_t t;
    std::memcpy(&t, d.bytes, 4);
    return t;
}

bool KademliaNode::checkToken(const InetAddress &requester, uint32_t token) const
{
    return token == makeToken(requester);
}

void KademliaNode::sendPacket(MsgType type,
                              uint32_t txid,
                              const InetAddress &to,
                              const std::string &payload)
{
    std::string pkt;
    putU32(&pkt, kMagic);
    pkt.push_back(static_cast<char>(kVersion));
    pkt.push_back(static_cast<char>(type));
    putU32(&pkt, txid);
    putU16(&pkt, 0);
    putU16(&pkt, 0);
    putId(&pkt, selfId_);
    putU16(&pkt, udp_->localAddress().toPort());
    putU16(&pkt, tcpPort_);
    pkt.append(payload);
    const ssize_t n = udp_->sendTo(pkt.data(), pkt.size(), to);
    if (n < 0)
    {
        LOG_ERROR("udp send type=%u to=%s failed\n",
                  static_cast<unsigned>(type),
                  to.toIpPort().c_str());
    }
}

void KademliaNode::onUdpMessage(const char *data, size_t len, const InetAddress &peer)
{
    const char *p = data;
    const char *end = data + len;
    uint32_t magic = 0;
    if (!getU32(p, end, &magic) || magic != kMagic || end - p < 30)
    {
        return;
    }
    const uint8_t ver = static_cast<uint8_t>(*p++);
    const auto type = static_cast<MsgType>(static_cast<uint8_t>(*p++));
    if (ver != kVersion)
    {
        return;
    }
    uint32_t txid = 0;
    uint16_t flags = 0, reserved = 0;
    if (!getU32(p, end, &txid) || !getU16(p, end, &flags) || !getU16(p, end, &reserved))
    {
        return;
    }
    (void)flags;
    (void)reserved;
    PeerContact from;
    if (!getId(p, end, &from.id) || !getU16(p, end, &from.udpPort) || !getU16(p, end, &from.tcpPort))
    {
        return;
    }
    from.ipv4 = peer.getSockAddr()->sin_addr.s_addr;
    from.udpPort = peer.toPort();
    table_.upsert(from);

    switch (type)
    {
    case kPing:
        handlePing(txid, peer);
        break;
    case kFindNode:
    {
        NodeId target;
        if (getId(p, end, &target))
            handleFindNode(txid, target, peer);
        break;
    }
    case kGetPeers:
    {
        InfoHash ih;
        if (getId(p, end, &ih))
            handleGetPeers(txid, ih, peer);
        break;
    }
    case kAnnounce:
    {
        InfoHash ih;
        uint16_t tcp = 0;
        uint32_t token = 0;
        if (getId(p, end, &ih) && getU16(p, end, &tcp) && getU32(p, end, &token))
            handleAnnounce(txid, from, ih, tcp, token, peer);
        break;
    }
    case kFindNodeResp:
    case kGetPeersResp:
    {
        auto it = pending_.find(txid);
        if (it == pending_.end())
            break;
        auto &rpc = it->second;
        if (rpc.inflight > 0)
        {
            --rpc.inflight;
        }
        uint32_t token = 0;
        if (type == kGetPeersResp)
        {
            if (!getU32(p, end, &token))
            {
                lookupStep(txid);
                break;
            }
            if (rpc.kind == kLookupForAnnounce || rpc.kind == kLookupGetPeers)
            {
                // 应答节点本身可作为下载候选（其 header 带 tcpPort）
                if (from.tcpPort > 0)
                {
                    rpc.peerResults.push_back(from);
                }
            }
            if (rpc.kind == kLookupForAnnounce)
            {
                sendAnnounceWithToken(peer, rpc.info, token);
            }
        }
        while (p + 28 <= end)
        {
            PeerContact c;
            if (!getContact(p, end, &c))
                break;
            table_.upsert(c);
            rpc.closest.push_back(c);
            if (type == kGetPeersResp && c.tcpPort > 0)
            {
                rpc.peerResults.push_back(c);
            }
        }
        lookupStep(txid);
        break;
    }
    default:
        break;
    }
}

void KademliaNode::handlePing(uint32_t txid, const InetAddress &peer)
{
    sendPacket(kPong, txid, peer, std::string());
}

void KademliaNode::handleFindNode(uint32_t txid, const NodeId &target, const InetAddress &peer)
{
    std::string body;
    for (const auto &c : table_.closest(target, kBucketSize))
        putContact(&body, c);
    sendPacket(kFindNodeResp, txid, peer, body);
}

void KademliaNode::handleGetPeers(uint32_t txid, const InfoHash &ih, const InetAddress &peer)
{
    std::string body;
    putU32(&body, makeToken(peer));
    const auto key = idHex(ih);
    auto it = peerStore_.find(key);
    size_t n = 0;
    if (it != peerStore_.end() && !it->second.empty())
    {
        for (const auto &c : it->second)
        {
            putContact(&body, c);
            ++n;
        }
    }
    else
    {
        // 未命中资源时只返回路由节点，tcpPort 置 0，避免被当成下载 peer。
        for (const auto &c : table_.closest(ih, kBucketSize))
        {
            PeerContact r = c;
            r.tcpPort = 0;
            putContact(&body, r);
        }
    }
    // 应答方自己若在做种，也附带一份（防止 peerStore 尚未写入）
    if (announcing_ && idEqual(announceHash_, ih))
    {
        putContact(&body, selfContact());
        ++n;
    }
    sendPacket(kGetPeersResp, txid, peer, body);
    LOG_INFO("handleGetPeers ih=%s storePeers=%zu from=%s\n",
             key.c_str(),
             n,
             peer.toIpPort().c_str());
}

void KademliaNode::handleAnnounce(uint32_t txid,
                                  const PeerContact &from,
                                  const InfoHash &ih,
                                  uint16_t tcpPort,
                                  uint32_t token,
                                  const InetAddress &peer)
{
    if (!checkToken(peer, token))
    {
        LOG_INFO("announce bad token from %s\n", peer.toIpPort().c_str());
        return;
    }
    PeerContact c = from;
    c.tcpPort = tcpPort;
    c.udpPort = peer.toPort();
    c.ipv4 = peer.getSockAddr()->sin_addr.s_addr;
    auto &list = peerStore_[idHex(ih)];
    bool found = false;
    for (auto &x : list)
    {
        if (idEqual(x.id, c.id) || (x.ipv4 == c.ipv4 && x.tcpPort == c.tcpPort))
        {
            x = c;
            found = true;
            break;
        }
    }
    if (!found)
        list.push_back(c);
    sendPacket(kAnnounceResp, txid, peer, std::string());
    LOG_INFO("announce stored ih=%s peer=%s:%u list=%zu\n",
             idHex(ih).c_str(),
             peer.toIp().c_str(),
             tcpPort,
             list.size());
}

void KademliaNode::sendAnnounceWithToken(const InetAddress &to, const InfoHash &ih, uint32_t token)
{
    std::string body;
    putId(&body, ih);
    putU16(&body, tcpPort_);
    putU32(&body, token);
    sendPacket(kAnnounce, nextTxid(), to, body);
}

void KademliaNode::bootstrap(const std::vector<InetAddress> &seeds)
{
    for (const auto &s : seeds)
    {
        PeerContact seed;
        seed.id = randomNodeId();
        seed.ipv4 = s.getSockAddr()->sin_addr.s_addr;
        seed.udpPort = s.toPort();
        seed.tcpPort = 0;
        table_.upsert(seed);
        sendPacket(kPing, nextTxid(), s, std::string());
    }
    startLookup(kLookupFindNode, selfId_, PeerListCallback());
}

void KademliaNode::startLookup(PendingKind kind, const NodeId &target, PeerListCallback cb)
{
    const uint32_t txid = nextTxid();
    PendingRpc rpc;
    rpc.kind = kind;
    rpc.queryType = (kind == kLookupFindNode) ? kFindNode : kGetPeers;
    rpc.target = target;
    rpc.info = target;
    rpc.cb = std::move(cb);
    rpc.closest = table_.closest(target, kBucketSize * 2);
    pending_[txid] = std::move(rpc);
    lookupStep(txid);
}

void KademliaNode::lookupStep(uint32_t txid)
{
    auto it = pending_.find(txid);
    if (it == pending_.end())
        return;
    auto &rpc = it->second;

    std::sort(rpc.closest.begin(), rpc.closest.end(), [&](const PeerContact &a, const PeerContact &b) {
        return closerThan(rpc.target, a.id, b.id);
    });
    {
        std::vector<PeerContact> uniq;
        for (const auto &c : rpc.closest)
        {
            bool dup = false;
            for (const auto &u : uniq)
            {
                if (idEqual(u.id, c.id))
                {
                    dup = true;
                    break;
                }
            }
            if (!dup && !idEqual(c.id, selfId_))
                uniq.push_back(c);
        }
        rpc.closest.swap(uniq);
    }

    int launched = 0;
    for (const auto &c : rpc.closest)
    {
        if (rpc.inflight + launched >= kAlpha)
            break;
        const std::string key = idHex(c.id);
        if (rpc.queried.count(key))
            continue;
        rpc.queried.insert(key);
        std::string body;
        putId(&body, rpc.target);
        sendPacket(rpc.queryType, txid, c.toUdpAddr(), body);
        ++launched;
    }
    rpc.inflight += launched;

    if (launched == 0 && rpc.inflight <= 0)
    {
        completeLookup(txid);
        return;
    }

    loop_->cancel(rpc.timer);
    rpc.timer = loop_->runAfter(1.2, [self = shared_from_this(), txid]() {
        self->onLookupTimeout(txid);
    });
}

void KademliaNode::onLookupTimeout(uint32_t txid)
{
    auto it = pending_.find(txid);
    if (it == pending_.end())
        return;
    it->second.inflight = 0;
    ++it->second.retries;
    if (it->second.retries >= 4)
    {
        completeLookup(txid);
        return;
    }
    lookupStep(txid);
}

void KademliaNode::completeLookup(uint32_t txid)
{
    auto it = pending_.find(txid);
    if (it == pending_.end())
        return;
    loop_->cancel(it->second.timer);
    PendingRpc rpc = std::move(it->second);
    pending_.erase(it);

    if (rpc.kind == kLookupGetPeers)
    {
        auto local = peerStore_.find(idHex(rpc.info));
        if (local != peerStore_.end())
        {
            rpc.peerResults.insert(rpc.peerResults.end(), local->second.begin(), local->second.end());
        }
        // 路由表中已暴露 tcpPort 的邻居也作为候选
        for (const auto &c : table_.all())
        {
            if (c.tcpPort > 0)
            {
                rpc.peerResults.push_back(c);
            }
        }
        std::vector<PeerContact> peers;
        for (const auto &c : rpc.peerResults)
        {
            if (c.tcpPort == 0 || idEqual(c.id, selfId_))
                continue;
            bool dup = false;
            for (const auto &p : peers)
            {
                if ((p.ipv4 == c.ipv4 && p.tcpPort == c.tcpPort) || idEqual(p.id, c.id))
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                peers.push_back(c);
        }
        LOG_INFO("get_peers ih=%s peers=%zu table=%zu\n",
                 idHex(rpc.info).c_str(),
                 peers.size(),
                 table_.size());
        if (rpc.cb)
            rpc.cb(peers);
    }
    else if (rpc.cb)
    {
        rpc.cb(rpc.closest);
    }
}

void KademliaNode::getPeers(const InfoHash &infoHash, PeerListCallback cb)
{
    startLookup(kLookupGetPeers, infoHash, std::move(cb));
}

void KademliaNode::announce(const InfoHash &infoHash)
{
    announcing_ = true;
    announceHash_ = infoHash;
    // 本地先登记自己
    auto self = selfContact();
    auto &list = peerStore_[idHex(infoHash)];
    bool found = false;
    for (auto &x : list)
    {
        if (idEqual(x.id, self.id))
        {
            x = self;
            found = true;
            break;
        }
    }
    if (!found)
        list.push_back(self);

    startLookup(kLookupForAnnounce, infoHash, PeerListCallback());
}

} // namespace p2p
} // namespace transport
