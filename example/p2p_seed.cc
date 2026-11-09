#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include <memory>

#include "EventLoop.h"
#include "InetAddress.h"
#include "Logger.h"
#include "Sha1.h"
#include "TorrentMeta.h"
#include "TorrentSession.h"

using transport::p2p::TorrentMeta;
using transport::p2p::TorrentSession;

int main(int argc, char *argv[])
{
    if (argc < 5)
    {
        std::printf("usage: %s <file> <metaOut> <udpPort> <tcpPort> [bootstrap udpPort]\n", argv[0]);
        return 1;
    }
    const std::string file = argv[1];
    const std::string metaPath = argv[2];
    const uint16_t udpPort = static_cast<uint16_t>(std::stoi(argv[3]));
    const uint16_t tcpPort = static_cast<uint16_t>(std::stoi(argv[4]));

    TorrentMeta meta;
    if (!TorrentMeta::createFromFile(file, &meta) || !meta.save(metaPath))
    {
        std::fprintf(stderr, "create meta failed\n");
        return 1;
    }
    std::printf("meta ih=%s pieces=%u size=%llu\n",
                transport::p2p::Sha1::toHex(meta.infoHash).c_str(),
                meta.pieceCount(),
                static_cast<unsigned long long>(meta.fileSize));

    EventLoop loop;
    const uint32_t adv = transport::p2p::detectAdvertiseIPv4();
    auto session = std::make_shared<TorrentSession>(&loop, meta, file, udpPort, tcpPort, true);
    session->setAdvertiseIPv4(adv);
    if (argc >= 6)
    {
        const uint16_t boot = static_cast<uint16_t>(std::stoi(argv[5]));
        session->setBootstrap({InetAddress(boot, transport::p2p::ipv4ToString(adv))});
    }
    std::printf("advertise_ip=%s\n", transport::p2p::ipv4ToString(adv).c_str());
    session->setProgressCallback([](const transport::p2p::SwarmProgress &p) {
        std::printf("\rseed have=%u/%u peers=%zu", p.have, p.total, p.peers);
        std::fflush(stdout);
    });
    session->start();
    std::printf("\np2p_seed udp=%u tcp=%u seeding...\n", udpPort, tcpPort);
    loop.loop();
    return 0;
}
