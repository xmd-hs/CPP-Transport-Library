#include "PeerWire.h"

#include <arpa/inet.h>
#include <cstring>

namespace transport
{
namespace p2p
{

std::string encodePeerFrame(PeerMsg type, const void *payload, size_t len)
{
    std::string out;
    out.resize(kPeerHeaderSize + len);
    const uint32_t magic = htonl(kPeerMagic);
    const uint32_t nlen = htonl(static_cast<uint32_t>(len));
    std::memcpy(&out[0], &magic, 4);
    out[4] = static_cast<char>(kPeerVersion);
    out[5] = static_cast<char>(type);
    std::memcpy(&out[6], &nlen, 4);
    if (payload && len)
    {
        std::memcpy(&out[10], payload, len);
    }
    return out;
}

void encodePiecePrefix(uint32_t index, uint32_t pieceBytes, char out[14])
{
    const uint32_t magic = htonl(kPeerMagic);
    const uint32_t nlen = htonl(4u + pieceBytes);
    const uint32_t nidx = htonl(index);
    std::memcpy(out, &magic, 4);
    out[4] = static_cast<char>(kPeerVersion);
    out[5] = static_cast<char>(PeerMsg::kPiece);
    std::memcpy(out + 6, &nlen, 4);
    std::memcpy(out + 10, &nidx, 4);
}

bool tryDecodePeerFrame(const char *data, size_t size, size_t *offset, PeerFrame *out)
{
    if (!data || !offset || !out)
    {
        return false;
    }
    const size_t off = *offset;
    if (size < off + kPeerHeaderSize)
    {
        return false;
    }
    const char *p = data + off;
    uint32_t magic = 0;
    std::memcpy(&magic, p, 4);
    magic = ntohl(magic);
    if (magic != kPeerMagic || static_cast<uint8_t>(p[4]) != kPeerVersion)
    {
        *offset = size;
        return false;
    }
    uint32_t len = 0;
    std::memcpy(&len, p + 6, 4);
    len = ntohl(len);
    if (len > 8 * 1024 * 1024)
    {
        *offset = size;
        return false;
    }
    if (size < off + kPeerHeaderSize + len)
    {
        return false;
    }
    out->type = static_cast<PeerMsg>(static_cast<uint8_t>(p[5]));
    out->payload = p + kPeerHeaderSize;
    out->payloadLen = len;
    *offset = off + kPeerHeaderSize + len;
    return true;
}

std::string encodeHandshake(const InfoHash &ih, const NodeId &peerId)
{
    char body[40];
    std::memcpy(body, ih.bytes, 20);
    std::memcpy(body + 20, peerId.bytes, 20);
    return encodePeerFrame(PeerMsg::kHandshake, body, 40);
}

bool decodeHandshake(const char *payload, size_t len, InfoHash *ih, NodeId *peerId)
{
    if (!payload || len < 40 || !ih || !peerId)
    {
        return false;
    }
    std::memcpy(ih->bytes, payload, 20);
    std::memcpy(peerId->bytes, payload + 20, 20);
    return true;
}

} // namespace p2p
} // namespace transport
