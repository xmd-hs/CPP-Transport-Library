#include "Sha1.h"

#include <algorithm>
#include <cstring>
#include <unistd.h>

#if defined(P2P_HAVE_OPENSSL)
#  include <openssl/sha.h>
#  define P2P_USE_OPENSSL_SHA1 1
#elif defined(__has_include)
#  if __has_include(<openssl/sha.h>)
#    include <openssl/sha.h>
#    define P2P_USE_OPENSSL_SHA1 1
#  endif
#endif

namespace transport
{
namespace p2p
{
namespace
{

#if !defined(P2P_USE_OPENSSL_SHA1)
uint32_t rol(uint32_t v, uint32_t n) { return (v << n) | (v >> (32 - n)); }
#endif

} // namespace

Sha1::Sha1()
    : bitLen_(0)
    , bufferLen_(0)
{
    state_[0] = 0x67452301;
    state_[1] = 0xEFCDAB89;
    state_[2] = 0x98BADCFE;
    state_[3] = 0x10325476;
    state_[4] = 0xC3D2E1F0;
}

void Sha1::transform(const uint8_t block[64])
{
#if defined(P2P_USE_OPENSSL_SHA1)
    (void)block;
#else
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
    {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i)
    {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];

    for (int i = 0; i < 80; ++i)
    {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
#endif
}

void Sha1::update(const void *data, size_t len)
{
#if defined(P2P_USE_OPENSSL_SHA1)
    (void)data;
    (void)len;
#else
    const auto *p = static_cast<const uint8_t *>(data);
    bitLen_ += static_cast<uint64_t>(len) * 8;
    while (len > 0)
    {
        const size_t n = std::min(len, 64 - bufferLen_);
        std::memcpy(buffer_ + bufferLen_, p, n);
        bufferLen_ += n;
        p += n;
        len -= n;
        if (bufferLen_ == 64)
        {
            transform(buffer_);
            bufferLen_ = 0;
        }
    }
#endif
}

Sha1Digest Sha1::final()
{
#if defined(P2P_USE_OPENSSL_SHA1)
    return Sha1Digest{};
#else
    uint8_t pad[64 + 8];
    size_t padLen = 0;
    pad[padLen++] = 0x80;
    const size_t total = bufferLen_ + 1;
    const size_t zeros = (total <= 56) ? (56 - total) : (120 - total);
    for (size_t i = 0; i < zeros; ++i)
    {
        pad[padLen++] = 0;
    }
    for (int i = 7; i >= 0; --i)
    {
        pad[padLen++] = static_cast<uint8_t>((bitLen_ >> (i * 8)) & 0xff);
    }
    update(pad, padLen);

    Sha1Digest out;
    for (int i = 0; i < 5; ++i)
    {
        out.bytes[i * 4] = static_cast<uint8_t>((state_[i] >> 24) & 0xff);
        out.bytes[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xff);
        out.bytes[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xff);
        out.bytes[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xff);
    }
    return out;
#endif
}

Sha1Digest Sha1::hash(const void *data, size_t len)
{
#if defined(P2P_USE_OPENSSL_SHA1)
    Sha1Digest out;
    ::SHA1(static_cast<const unsigned char *>(data), len, out.bytes);
    return out;
#else
    Sha1 s;
    s.update(data, len);
    return s.final();
#endif
}

Sha1Digest Sha1::hashFilePiece(int fd, uint64_t offset, uint64_t length)
{
#if defined(P2P_USE_OPENSSL_SHA1)
    SHA_CTX ctx;
    SHA1_Init(&ctx);
    uint8_t buf[64 * 1024];
    uint64_t left = length;
    uint64_t off = offset;
    while (left > 0)
    {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(left, sizeof(buf)));
        const ssize_t r = ::pread(fd, buf, n, static_cast<off_t>(off));
        if (r <= 0)
        {
            break;
        }
        SHA1_Update(&ctx, buf, static_cast<size_t>(r));
        off += static_cast<uint64_t>(r);
        left -= static_cast<uint64_t>(r);
    }
    Sha1Digest out;
    SHA1_Final(out.bytes, &ctx);
    return out;
#else
    Sha1 s;
    uint8_t buf[64 * 1024];
    uint64_t left = length;
    uint64_t off = offset;
    while (left > 0)
    {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(left, sizeof(buf)));
        const ssize_t r = ::pread(fd, buf, n, static_cast<off_t>(off));
        if (r <= 0)
        {
            break;
        }
        s.update(buf, static_cast<size_t>(r));
        off += static_cast<uint64_t>(r);
        left -= static_cast<uint64_t>(r);
    }
    return s.final();
#endif
}

std::string Sha1::toHex(const Sha1Digest &d)
{
    static const char *kHex = "0123456789abcdef";
    std::string out(40, '0');
    for (int i = 0; i < 20; ++i)
    {
        out[i * 2] = kHex[(d.bytes[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHex[d.bytes[i] & 0xf];
    }
    return out;
}

} // namespace p2p
} // namespace transport
