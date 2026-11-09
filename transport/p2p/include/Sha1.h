#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace transport
{
namespace p2p
{

struct Sha1Digest
{
    uint8_t bytes[20]{};
};

class Sha1
{
public:
    Sha1();
    void update(const void *data, size_t len);
    Sha1Digest final();

    static Sha1Digest hash(const void *data, size_t len);
    static Sha1Digest hashFilePiece(int fd, uint64_t offset, uint64_t length);
    static std::string toHex(const Sha1Digest &d);

private:
    void transform(const uint8_t block[64]);

    uint32_t state_[5];
    uint64_t bitLen_;
    uint8_t buffer_[64];
    size_t bufferLen_;
};

} // namespace p2p
} // namespace transport
