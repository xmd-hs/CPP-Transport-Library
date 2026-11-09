#include "Codec.h"

#include <cstring>

namespace transport
{
namespace
{

bool bodyInsideFrame(MessageType type)
{
    return type == MessageType::kError;
}

uint32_t computeChecksum(const TransferHeader &header,
                         const char *extra,
                         size_t extraLen,
                         const void *body,
                         size_t bodyLen)
{
    TransferHeader tmp = header;
    tmp.checksum = 0;
    uint32_t crc = protocol::crc32(&tmp, sizeof(tmp));
    if (extraLen > 0)
    {
        crc = protocol::crc32Append(crc, extra, extraLen);
    }
    if (body != nullptr && bodyLen > 0)
    {
        crc = protocol::crc32Append(crc, body, bodyLen);
    }
    return crc;
}

} // namespace

DecodeStatus Codec::tryDecode(Buffer *buf, DecodedFrame *out)
{
    if (buf->readableBytes() < protocol::kHeaderSize)
    {
        return DecodeStatus::kNeedMore;
    }

    TransferHeader header;
    std::memcpy(&header, buf->peek(), protocol::kHeaderSize);

    if (header.magic != protocol::kMagic)
    {
        return DecodeStatus::kError;
    }
    if (header.version != protocol::kVersion)
    {
        return DecodeStatus::kError;
    }
    if (header.headerExtra > protocol::kMaxPathLen + protocol::kMaxTokenLen + 1)
    {
        return DecodeStatus::kError;
    }

    const MessageType type = static_cast<MessageType>(header.type);
    size_t inFrameBody = 0;
    if (bodyInsideFrame(type))
    {
        if (header.bodyLen > protocol::kMaxErrorLen)
        {
            return DecodeStatus::kError;
        }
        inFrameBody = static_cast<size_t>(header.bodyLen);
    }

    const size_t frameLen =
        protocol::kHeaderSize + static_cast<size_t>(header.headerExtra) + inFrameBody;
    if (buf->readableBytes() < frameLen)
    {
        return DecodeStatus::kNeedMore;
    }

    const char *extraPtr = buf->peek() + protocol::kHeaderSize;
    const char *bodyPtr = extraPtr + header.headerExtra;
    const uint32_t expect = computeChecksum(
        header,
        extraPtr, header.headerExtra,
        inFrameBody > 0 ? bodyPtr : nullptr,
        inFrameBody);
    if (expect != header.checksum)
    {
        return DecodeStatus::kError;
    }

    buf->retrieve(protocol::kHeaderSize);
    out->header = header;
    out->extra.assign(buf->peek(), header.headerExtra);
    buf->retrieve(header.headerExtra);
    out->body.clear();
    if (inFrameBody > 0)
    {
        out->body.assign(buf->peek(), inFrameBody);
        buf->retrieve(inFrameBody);
    }

    if (!protocol::decodeExtra(out->extra, &out->token, &out->path))
    {
        return DecodeStatus::kError;
    }
    return DecodeStatus::kOk;
}

void Codec::appendFrame(Buffer *buf,
                        MessageType type,
                        const std::string &extra,
                        const void *body,
                        size_t bodyLen,
                        uint64_t offset,
                        uint64_t fileSize,
                        uint16_t flags)
{
    TransferHeader header;
    protocol::encodeHeader(&header,
                           type,
                           static_cast<uint32_t>(extra.size()),
                           bodyLen,
                           offset,
                           fileSize,
                           flags);
    const size_t inBody = bodyInsideFrame(type) ? bodyLen : 0;
    header.checksum = computeChecksum(header, extra.data(), extra.size(), body, inBody);

    buf->append(reinterpret_cast<const char *>(&header), protocol::kHeaderSize);
    if (!extra.empty())
    {
        buf->append(extra.data(), extra.size());
    }
    if (inBody > 0 && body != nullptr)
    {
        buf->append(static_cast<const char *>(body), inBody);
    }
}

std::string Codec::encodeFrame(MessageType type,
                               const std::string &extra,
                               const void *body,
                               size_t bodyLen,
                               uint64_t offset,
                               uint64_t fileSize,
                               uint16_t flags)
{
    Buffer buf;
    appendFrame(&buf, type, extra, body, bodyLen, offset, fileSize, flags);
    return buf.retrieveAllAsString();
}

} // namespace transport
