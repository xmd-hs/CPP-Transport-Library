#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "InetAddress.h"
#include "Sha1.h"

namespace transport
{
namespace p2p
{

constexpr size_t kIdBytes = 20;
constexpr int kBucketCount = 160;
constexpr int kBucketSize = 8;
constexpr int kAlpha = 3;

using NodeId = Sha1Digest;
using InfoHash = Sha1Digest;

inline bool idEqual(const NodeId &a, const NodeId &b)
{
    return std::memcmp(a.bytes, b.bytes, kIdBytes) == 0;
}

inline NodeId xorDistance(const NodeId &a, const NodeId &b)
{
    NodeId out;
    for (size_t i = 0; i < kIdBytes; ++i)
    {
        out.bytes[i] = static_cast<uint8_t>(a.bytes[i] ^ b.bytes[i]);
    }
    return out;
}

// 最高非零位所在的桶下标；全 0 返回 -1。
inline int bucketIndex(const NodeId &distance)
{
    for (size_t i = 0; i < kIdBytes; ++i)
    {
        const uint8_t v = distance.bytes[i];
        if (v == 0)
        {
            continue;
        }
        int bit = 7;
        while (((v >> bit) & 1) == 0)
        {
            --bit;
        }
        return static_cast<int>((kIdBytes - 1 - i) * 8 + bit);
    }
    return -1;
}

inline bool closerThan(const NodeId &target, const NodeId &a, const NodeId &b)
{
    const NodeId da = xorDistance(target, a);
    const NodeId db = xorDistance(target, b);
    return std::memcmp(da.bytes, db.bytes, kIdBytes) < 0;
}

NodeId randomNodeId();
NodeId nodeIdFromSeed(const std::string &seed);

// 选择用于 DHT/peer 宣告的 IPv4（网络序）。优先环境变量 P2P_ADVERTISE_IP，
// 否则用 UDP connect 探测；失败则回环。部分 WSL 网络模式下 127.0.0.1 UDP 不可用。
uint32_t detectAdvertiseIPv4();
std::string ipv4ToString(uint32_t be);

struct PeerContact
{
    NodeId id{};
    uint32_t ipv4 = 0; // network order
    uint16_t udpPort = 0;
    uint16_t tcpPort = 0;

    InetAddress toUdpAddr() const;
    InetAddress toTcpAddr() const;
    static PeerContact fromAddr(const NodeId &id,
                                const InetAddress &udp,
                                uint16_t tcpPort);
};

} // namespace p2p
} // namespace transport
