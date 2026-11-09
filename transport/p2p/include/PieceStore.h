#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

#include "TorrentMeta.h"

namespace transport
{
namespace p2p
{

// 约定：同一 EventLoop 线程访问，无内部锁。
class PieceStore
{
public:
    PieceStore();
    ~PieceStore();

    bool open(const TorrentMeta &meta, const std::string &filePath, bool expectComplete);
    void close();

    bool hasPiece(uint32_t index) const;
    bool complete() const;
    uint32_t haveCount() const;
    std::string bitfield() const;

    bool readPiece(uint32_t index, std::string *out) const;
    bool writePiece(uint32_t index, const void *data, size_t len);

    int fd() const { return fd_; }
    off_t pieceOffset(uint32_t index) const;
    uint32_t pieceSize(uint32_t index) const { return meta_.pieceSize(index); }

    const TorrentMeta &meta() const { return meta_; }

private:
    TorrentMeta meta_;
    int fd_;
    std::vector<uint8_t> have_;
    uint32_t haveCount_;
};

} // namespace p2p
} // namespace transport
