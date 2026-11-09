#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

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
    if (argc < 6)
    {
        std::printf("usage: %s <meta> <outFile> <udpPort> <tcpPort> <bootstrapUdpPort>\n", argv[0]);
        return 1;
    }
    const std::string metaPath = argv[1];
    const std::string outFile = argv[2];
    const uint16_t udpPort = static_cast<uint16_t>(std::stoi(argv[3]));
    const uint16_t tcpPort = static_cast<uint16_t>(std::stoi(argv[4]));
    const uint16_t bootUdp = static_cast<uint16_t>(std::stoi(argv[5]));

    TorrentMeta meta;
    if (!TorrentMeta::load(metaPath, &meta))
    {
        std::fprintf(stderr, "load meta failed\n");
        return 1;
    }
    std::printf("leech ih=%s pieces=%u\n",
                transport::p2p::Sha1::toHex(meta.infoHash).c_str(),
                meta.pieceCount());

    EventLoop loop;
    std::atomic_bool done{false};
    std::atomic_bool okFlag{false};

    const uint32_t adv = transport::p2p::detectAdvertiseIPv4();
    auto session =
        std::make_shared<TorrentSession>(&loop, meta, outFile, udpPort, tcpPort, false);
    session->setAdvertiseIPv4(adv);
    session->setBootstrap({InetAddress(bootUdp, transport::p2p::ipv4ToString(adv))});
    std::printf("advertise_ip=%s bootstrap=%s:%u\n",
                transport::p2p::ipv4ToString(adv).c_str(),
                transport::p2p::ipv4ToString(adv).c_str(),
                bootUdp);
    session->setProgressCallback([](const transport::p2p::SwarmProgress &p) {
        std::printf("\rleech have=%u/%u peers=%zu", p.have, p.total, p.peers);
        std::fflush(stdout);
    });
    session->setDoneCallback([&](bool ok, const std::string &msg) {
        std::printf("\ndone ok=%d msg=%s\n", ok ? 1 : 0, msg.c_str());
        okFlag = ok;
        done = true;
        loop.quit();
    });
    session->start();

    // 超时保护
    loop.runAfter(120.0, [&]() {
        if (!done)
        {
            std::printf("\ntimeout\n");
            loop.quit();
        }
    });
    loop.loop();
    session->stop();
    return okFlag ? 0 : 2;
}
