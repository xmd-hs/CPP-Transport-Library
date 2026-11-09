#pragma once

#include <cstdint>
#include <string>

#include "NodeId.h"

namespace transport
{
namespace p2p
{

enum class PeerMsg : uint8_t
{
    kHandshake = 1,
    kBitfield = 2,
    kHave = 3,
    kRequest = 4,
    kPiece = 5,
    kCancel = 6,
    kKeepalive = 7,
};

// payload 指向调用方缓冲内的视图，调用方在消费缓冲前必须处理完。
struct PeerFrame
{
    PeerMsg type = PeerMsg::kKeepalive;
    const char *payload = nullptr;
    size_t payloadLen = 0;
};

constexpr uint32_t kPeerMagic = 0x434C5050u; // 'CLPP'
constexpr uint8_t kPeerVersion = 1;
constexpr size_t kPeerHeaderSize = 10; // magic4 + ver1 + type1 + len4

std::string encodePeerFrame(PeerMsg type, const void *payload, size_t len);
// 从 data[*offset] 解析一帧；成功推进 *offset。需要更多数据返回 false。
// 坏帧时把 *offset 推到 size（调用方应丢弃整段）。
bool tryDecodePeerFrame(const char *data, size_t size, size_t *offset, PeerFrame *out);

void encodePiecePrefix(uint32_t index, uint32_t pieceBytes, char out[14]);

std::string encodeHandshake(const InfoHash &ih, const NodeId &peerId);
bool decodeHandshake(const char *payload, size_t len, InfoHash *ih, NodeId *peerId);

} // namespace p2p
} // namespace transport
