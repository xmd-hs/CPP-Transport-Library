#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "InetAddress.h"
#include "NodeId.h"
#include "RoutingTable.h"
#include "TimerId.h"
#include "UdpSocket.h"

class EventLoop;

namespace transport
{
namespace p2p
{

class KademliaNode : public std::enable_shared_from_this<KademliaNode>
{
public:
    using PeerListCallback = std::function<void(const std::vector<PeerContact> &)>;

    KademliaNode(EventLoop *loop,
                 const InetAddress &udpBind,
                 uint16_t advertiseTcpPort,
                 NodeId selfId = NodeId());

    void setAdvertiseIPv4(uint32_t be) { advertiseIPv4_ = be; }
    uint32_t advertiseIPv4() const { return advertiseIPv4_; }

    void start();
    void stop();

    const NodeId &selfId() const { return selfId_; }
    PeerContact selfContact() const;
    uint16_t udpPort() const { return udp_->localAddress().toPort(); }
    uint16_t tcpPort() const { return tcpPort_; }
    size_t tableSize() const { return table_.size(); }
    std::vector<PeerContact> knownNodes() const { return table_.all(); }

    void bootstrap(const std::vector<InetAddress> &seeds);
    void getPeers(const InfoHash &infoHash, PeerListCallback cb);
    void announce(const InfoHash &infoHash);

private:
    enum MsgType : uint8_t
    {
        kPing = 1,
        kPong = 2,
        kFindNode = 3,
        kFindNodeResp = 4,
        kGetPeers = 5,
        kGetPeersResp = 6,
        kAnnounce = 7,
        kAnnounceResp = 8,
    };

    enum PendingKind
    {
        kLookupFindNode,
        kLookupGetPeers,
        kLookupForAnnounce, // get_peers 仅为拿 token 再 announce
    };

    struct PendingRpc
    {
        PendingKind kind = kLookupFindNode;
        MsgType queryType = kFindNode;
        NodeId target{};
        InfoHash info{};
        int retries = 0;
        TimerId timer;
        PeerListCallback cb;
        std::vector<PeerContact> closest;
        std::unordered_set<std::string> queried; // id hex
        int inflight = 0;
        std::vector<PeerContact> peerResults; // tcp peers from store responses
    };

    void onUdpMessage(const char *data, size_t len, const InetAddress &peer);
    void sendPacket(MsgType type,
                    uint32_t txid,
                    const InetAddress &to,
                    const std::string &payload);

    void handlePing(uint32_t txid, const InetAddress &peer);
    void handleFindNode(uint32_t txid, const NodeId &target, const InetAddress &peer);
    void handleGetPeers(uint32_t txid, const InfoHash &ih, const InetAddress &peer);
    void handleAnnounce(uint32_t txid,
                        const PeerContact &from,
                        const InfoHash &ih,
                        uint16_t tcpPort,
                        uint32_t token,
                        const InetAddress &peer);

    void startLookup(PendingKind kind, const NodeId &target, PeerListCallback cb);
    void lookupStep(uint32_t txid);
    void onLookupTimeout(uint32_t txid);
    void completeLookup(uint32_t txid);

    void sendAnnounceWithToken(const InetAddress &to, const InfoHash &ih, uint32_t token);

    uint32_t makeToken(const InetAddress &requester) const;
    bool checkToken(const InetAddress &requester, uint32_t token) const;
    uint32_t nextTxid();
    static std::string idHex(const NodeId &id);

    static constexpr uint32_t kMagic = 0x4B444854u; // KDHT
    static constexpr uint8_t kVersion = 1;

    EventLoop *loop_;
    NodeId selfId_;
    uint16_t tcpPort_;
    uint32_t advertiseIPv4_ = 0; // network order; 0 => 从 local/loopback 推断
    std::unique_ptr<UdpSocket> udp_;
    RoutingTable table_;
    uint32_t nextTxid_;
    uint32_t tokenSecret_;
    std::map<uint32_t, PendingRpc> pending_;
    std::unordered_map<std::string, std::vector<PeerContact>> peerStore_;
    TimerId refreshTimer_;
    TimerId announceTimer_;
    InfoHash announceHash_{};
    bool announcing_ = false;
};

} // namespace p2p
} // namespace transport
