#include "NodeId.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <random>

namespace transport
{
namespace p2p
{

NodeId randomNodeId()
{
    NodeId id;
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0)
    {
        ::read(fd, id.bytes, kIdBytes);
        ::close(fd);
        return id;
    }
    std::mt19937_64 rng(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (size_t i = 0; i < kIdBytes; ++i)
    {
        id.bytes[i] = static_cast<uint8_t>(rng() & 0xff);
    }
    return id;
}

NodeId nodeIdFromSeed(const std::string &seed)
{
    return Sha1::hash(seed.data(), seed.size());
}

InetAddress PeerContact::toUdpAddr() const
{
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(udpPort);
    a.sin_addr.s_addr = ipv4;
    return InetAddress(a);
}

InetAddress PeerContact::toTcpAddr() const
{
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(tcpPort);
    a.sin_addr.s_addr = ipv4;
    return InetAddress(a);
}

PeerContact PeerContact::fromAddr(const NodeId &id, const InetAddress &udp, uint16_t tcpPort)
{
    PeerContact c;
    c.id = id;
    c.ipv4 = udp.getSockAddr()->sin_addr.s_addr;
    c.udpPort = udp.toPort();
    c.tcpPort = tcpPort;
    return c;
}

std::string ipv4ToString(uint32_t be)
{
    char buf[INET_ADDRSTRLEN] = {0};
    in_addr a;
    a.s_addr = be;
    ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

uint32_t detectAdvertiseIPv4()
{
    if (const char *env = std::getenv("P2P_ADVERTISE_IP"))
    {
        const in_addr_t a = ::inet_addr(env);
        if (a != INADDR_NONE && a != INADDR_ANY)
        {
            return a;
        }
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0)
    {
        sockaddr_in dst;
        std::memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = htons(53);
        dst.sin_addr.s_addr = ::inet_addr("8.8.8.8");
        if (::connect(fd, reinterpret_cast<sockaddr *>(&dst), sizeof(dst)) == 0)
        {
            sockaddr_in local;
            socklen_t len = sizeof(local);
            if (::getsockname(fd, reinterpret_cast<sockaddr *>(&local), &len) == 0 &&
                local.sin_addr.s_addr != 0 &&
                local.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            {
                const uint32_t ip = local.sin_addr.s_addr;
                ::close(fd);
                return ip;
            }
        }
        ::close(fd);
    }
    return htonl(INADDR_LOOPBACK);
}

} // namespace p2p
} // namespace transport
