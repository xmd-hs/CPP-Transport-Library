#include "SdkInternal.h"

namespace cldt_sdk
{
uint16_t get16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

uint32_t get32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t get64(const uint8_t *p)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
    {
        value = (value << 8) | p[i];
    }
    return value;
}

void put16(uint8_t *p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t *p, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        p[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

void put64(uint8_t *p, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
    {
        p[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

void encodeHeader(uint8_t raw[transport::protocol::kHeaderSize],
                  uint8_t type,
                  uint16_t flags,
                  uint32_t extraLen,
                  uint64_t bodyLen,
                  uint64_t offset,
                  uint64_t fileSize,
                  uint32_t checksum)
{
    std::memset(raw, 0, transport::protocol::kHeaderSize);
    put32(raw, transport::protocol::kMagic);
    raw[4] = transport::protocol::kVersion;
    raw[5] = type;
    put16(raw + 6, flags);
    put32(raw + 8, extraLen);
    put64(raw + 12, bodyLen);
    put64(raw + 20, offset);
    put64(raw + 28, fileSize);
    put32(raw + 36, checksum);
}

bool sendFrame(Stream &socket,
               uint8_t type,
               const std::string &extra,
               const std::string &body,
               uint64_t bodyLen,
               uint64_t offset,
               uint64_t fileSize,
               uint16_t flags)
{
    const bool bodyInFrame = type == static_cast<uint8_t>(transport::MessageType::kError);
    const size_t encodedBodyLength = bodyInFrame ? body.size() : 0;
    uint8_t raw[transport::protocol::kHeaderSize];
    encodeHeader(raw, type, flags, static_cast<uint32_t>(extra.size()), bodyLen,
                 offset, fileSize, 0);
    uint32_t checksum = transport::protocol::crc32(raw, sizeof(raw));
    if (!extra.empty())
    {
        checksum = transport::protocol::crc32Append(checksum, extra.data(), extra.size());
    }
    if (encodedBodyLength > 0)
    {
        checksum = transport::protocol::crc32Append(checksum, body.data(), encodedBodyLength);
    }
    put32(raw + 36, checksum);
    return sendAll(socket, raw, sizeof(raw)) &&
           (extra.empty() || sendAll(socket, extra.data(), extra.size())) &&
           (encodedBodyLength == 0 || sendAll(socket, body.data(), encodedBodyLength));
}

bool receiveFrame(Stream &socket, Frame *frame)
{
    uint8_t raw[transport::protocol::kHeaderSize];
    if (!receiveExact(socket, raw, sizeof(raw)) ||
        get32(raw) != transport::protocol::kMagic || raw[4] != transport::protocol::kVersion)
    {
        return false;
    }

    const uint32_t extraLen = get32(raw + 8);
    const uint64_t bodyLen = get64(raw + 12);
    const uint8_t type = raw[5];
    if ((get16(raw + 6) & ~transport::kFlagFinal) != 0) return false;
    if (type == static_cast<uint8_t>(transport::MessageType::kError) &&
        bodyLen > transport::protocol::kMaxErrorLen) return false;
    const size_t encodedBodyLength =
        type == static_cast<uint8_t>(transport::MessageType::kError)
            ? static_cast<size_t>(bodyLen)
            : 0;
    if (extraLen > transport::protocol::kMaxPathLen + transport::protocol::kMaxTokenLen + 1 ||
        encodedBodyLength > transport::protocol::kMaxErrorLen)
    {
        return false;
    }

    frame->type = type;
    frame->flags = get16(raw + 6);
    frame->bodyLen = bodyLen;
    frame->offset = get64(raw + 20);
    frame->fileSize = get64(raw + 28);
    frame->extra.resize(extraLen);
    frame->body.resize(encodedBodyLength);
    if ((extraLen && !receiveExact(socket, &frame->extra[0], extraLen)) ||
        (encodedBodyLength && !receiveExact(socket, &frame->body[0], encodedBodyLength)))
    {
        return false;
    }

    const uint32_t expected = get32(raw + 36);
    put32(raw + 36, 0);
    uint32_t checksum = transport::protocol::crc32(raw, sizeof(raw));
    if (extraLen)
    {
        checksum = transport::protocol::crc32Append(checksum, frame->extra.data(), extraLen);
    }
    if (encodedBodyLength)
    {
        checksum = transport::protocol::crc32Append(
            checksum, frame->body.data(), encodedBodyLength);
    }
    return checksum == expected;
}


} // namespace cldt_sdk
