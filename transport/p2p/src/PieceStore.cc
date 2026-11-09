#include "PieceStore.h"

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Logger.h"
#include "Sha1.h"

namespace transport
{
namespace p2p
{

PieceStore::PieceStore()
    : fd_(-1)
    , haveCount_(0)
{
}

PieceStore::~PieceStore()
{
    close();
}

bool PieceStore::open(const TorrentMeta &meta, const std::string &filePath, bool expectComplete)
{
    close();
    meta_ = meta;
    have_.assign((meta.pieceCount() + 7) / 8, 0);
    haveCount_ = 0;

    fd_ = ::open(filePath.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0)
    {
        return false;
    }
    if (::ftruncate(fd_, static_cast<off_t>(meta.fileSize)) < 0)
    {
        close();
        return false;
    }
#ifdef __linux__
    // 提示内核顺序读，利于做种 sendfile / 校验扫盘。
    (void)::posix_fadvise(fd_, 0, static_cast<off_t>(meta.fileSize),
                         POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED);
#endif

    for (uint32_t i = 0; i < meta.pieceCount(); ++i)
    {
        const uint32_t len = meta.pieceSize(i);
        const auto dig = Sha1::hashFilePiece(fd_, pieceOffset(i), len);
        if (std::memcmp(dig.bytes, meta.pieceHashes[i].bytes, 20) != 0)
        {
            if (expectComplete)
            {
                LOG_ERROR("PieceStore expectComplete hash mismatch piece=%u\n", i);
                close();
                return false;
            }
            continue;
        }
        have_[i / 8] |= static_cast<uint8_t>(1u << (7 - (i % 8)));
        ++haveCount_;
    }
    return true;
}

void PieceStore::close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
    have_.clear();
    haveCount_ = 0;
}

bool PieceStore::hasPiece(uint32_t index) const
{
    if (index >= meta_.pieceCount())
    {
        return false;
    }
    return (have_[index / 8] & (1u << (7 - (index % 8)))) != 0;
}

bool PieceStore::complete() const
{
    return haveCount_ == meta_.pieceCount() && meta_.pieceCount() > 0;
}

uint32_t PieceStore::haveCount() const
{
    return haveCount_;
}

std::string PieceStore::bitfield() const
{
    return std::string(reinterpret_cast<const char *>(have_.data()), have_.size());
}

off_t PieceStore::pieceOffset(uint32_t index) const
{
    return static_cast<off_t>(static_cast<uint64_t>(index) * meta_.pieceLength);
}

bool PieceStore::readPiece(uint32_t index, std::string *out) const
{
    if (fd_ < 0 || index >= meta_.pieceCount())
    {
        return false;
    }
    const uint32_t len = meta_.pieceSize(index);
    const off_t off = pieceOffset(index);
    out->assign(len, '\0');
    size_t got = 0;
    while (got < len)
    {
        const ssize_t n = ::pread(fd_, &(*out)[got], len - got, off + static_cast<off_t>(got));
        if (n <= 0)
        {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool PieceStore::writePiece(uint32_t index, const void *data, size_t len)
{
    if (fd_ < 0 || index >= meta_.pieceCount() || len != meta_.pieceSize(index))
    {
        return false;
    }
    const auto dig = Sha1::hash(data, len);
    if (std::memcmp(dig.bytes, meta_.pieceHashes[index].bytes, 20) != 0)
    {
        return false;
    }
    const off_t off = pieceOffset(index);
    size_t wrote = 0;
    const char *p = static_cast<const char *>(data);
    while (wrote < len)
    {
        const ssize_t n = ::pwrite(fd_, p + wrote, len - wrote, off + static_cast<off_t>(wrote));
        if (n <= 0)
        {
            return false;
        }
        wrote += static_cast<size_t>(n);
    }
    if ((have_[index / 8] & (1u << (7 - (index % 8)))) == 0)
    {
        have_[index / 8] |= static_cast<uint8_t>(1u << (7 - (index % 8)));
        ++haveCount_;
    }
    return true;
}

} // namespace p2p
} // namespace transport
