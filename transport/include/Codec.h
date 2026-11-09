#pragma once

#include <string>

#include "Buffer.h"
#include "Protocol.h"

namespace transport
{

struct DecodedFrame
{
    TransferHeader header;
    std::string extra;
    std::string body;
    std::string token;
    std::string path;
};

enum class DecodeStatus
{
    kNeedMore,
    kOk,
    kError,
};

class Codec
{
public:
    static DecodeStatus tryDecode(Buffer *buf, DecodedFrame *out);

    static void appendFrame(Buffer *buf,
                            MessageType type,
                            const std::string &extra,
                            const void *body,
                            size_t bodyLen,
                            uint64_t offset,
                            uint64_t fileSize,
                            uint16_t flags = kFlagNone);

    static std::string encodeFrame(MessageType type,
                                   const std::string &extra,
                                   const void *body,
                                   size_t bodyLen,
                                   uint64_t offset,
                                   uint64_t fileSize,
                                   uint16_t flags = kFlagNone);
};

} // namespace transport
