#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "NodeId.h"
#include "Sha1.h"

namespace transport
{
namespace p2p
{

struct TorrentMeta
{
    static constexpr uint32_t kMagic = 0x4D455441u; // 'META'
    static constexpr uint32_t kPieceLengthDefault = 1024 * 1024; // 1MiB，减少帧与 SHA 次数

    InfoHash infoHash{};
    std::string fileName;
    uint64_t fileSize = 0;
    uint32_t pieceLength = kPieceLengthDefault;
    std::vector<Sha1Digest> pieceHashes;

    uint32_t pieceCount() const { return static_cast<uint32_t>(pieceHashes.size()); }
    uint32_t pieceSize(uint32_t index) const;

    bool save(const std::string &path) const;
    static bool load(const std::string &path, TorrentMeta *out);
    static bool createFromFile(const std::string &filePath,
                               TorrentMeta *out,
                               uint32_t pieceLength = kPieceLengthDefault);
};

} // namespace p2p
} // namespace transport
