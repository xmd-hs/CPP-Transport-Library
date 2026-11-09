#include "TorrentMeta.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>

#include "Logger.h"

namespace transport
{
namespace p2p
{

uint32_t TorrentMeta::pieceSize(uint32_t index) const
{
    if (index + 1 < pieceCount())
    {
        return pieceLength;
    }
    const uint64_t rem = fileSize % pieceLength;
    return rem == 0 ? pieceLength : static_cast<uint32_t>(rem);
}

bool TorrentMeta::createFromFile(const std::string &filePath, TorrentMeta *out, uint32_t pieceLength)
{
    const int fd = ::open(filePath.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
    {
        ::close(fd);
        return false;
    }

    out->fileName = filePath.substr(filePath.find_last_of('/') == std::string::npos
                                        ? 0
                                        : filePath.find_last_of('/') + 1);
    out->fileSize = static_cast<uint64_t>(st.st_size);
    out->pieceLength = pieceLength;
    out->pieceHashes.clear();

    const uint32_t count = static_cast<uint32_t>(
        (out->fileSize + pieceLength - 1) / pieceLength);
    out->pieceHashes.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint64_t off = static_cast<uint64_t>(i) * pieceLength;
        const uint64_t len = std::min<uint64_t>(pieceLength, out->fileSize - off);
        out->pieceHashes.push_back(Sha1::hashFilePiece(fd, off, len));
    }
    ::close(fd);

    // infoHash = sha1(fileName|fileSize|pieceLength|hashes)
    Sha1 s;
    s.update(out->fileName.data(), out->fileName.size());
    s.update(&out->fileSize, sizeof(out->fileSize));
    s.update(&out->pieceLength, sizeof(out->pieceLength));
    for (const auto &h : out->pieceHashes)
    {
        s.update(h.bytes, 20);
    }
    out->infoHash = s.final();
    return true;
}

bool TorrentMeta::save(const std::string &path) const
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
    {
        return false;
    }
    const uint32_t magic = kMagic;
    const uint32_t ver = 1;
    const uint32_t nameLen = static_cast<uint32_t>(fileName.size());
    const uint32_t count = pieceCount();
    ofs.write(reinterpret_cast<const char *>(&magic), 4);
    ofs.write(reinterpret_cast<const char *>(&ver), 4);
    ofs.write(reinterpret_cast<const char *>(infoHash.bytes), 20);
    ofs.write(reinterpret_cast<const char *>(&nameLen), 4);
    ofs.write(fileName.data(), nameLen);
    ofs.write(reinterpret_cast<const char *>(&fileSize), 8);
    ofs.write(reinterpret_cast<const char *>(&pieceLength), 4);
    ofs.write(reinterpret_cast<const char *>(&count), 4);
    for (const auto &h : pieceHashes)
    {
        ofs.write(reinterpret_cast<const char *>(h.bytes), 20);
    }
    return static_cast<bool>(ofs);
}

bool TorrentMeta::load(const std::string &path, TorrentMeta *out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
    {
        return false;
    }
    uint32_t magic = 0, ver = 0, nameLen = 0, count = 0;
    ifs.read(reinterpret_cast<char *>(&magic), 4);
    ifs.read(reinterpret_cast<char *>(&ver), 4);
    if (magic != kMagic || ver != 1)
    {
        return false;
    }
    ifs.read(reinterpret_cast<char *>(out->infoHash.bytes), 20);
    ifs.read(reinterpret_cast<char *>(&nameLen), 4);
    if (nameLen > 4096)
    {
        return false;
    }
    out->fileName.assign(nameLen, '\0');
    ifs.read(&out->fileName[0], nameLen);
    ifs.read(reinterpret_cast<char *>(&out->fileSize), 8);
    ifs.read(reinterpret_cast<char *>(&out->pieceLength), 4);
    ifs.read(reinterpret_cast<char *>(&count), 4);
    out->pieceHashes.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        ifs.read(reinterpret_cast<char *>(out->pieceHashes[i].bytes), 20);
    }
    return static_cast<bool>(ifs);
}

} // namespace p2p
} // namespace transport
