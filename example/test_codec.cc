#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "Buffer.h"
#include "Codec.h"
#include "Protocol.h"

using transport::Codec;
using transport::DecodeStatus;
using transport::DecodedFrame;
using transport::MessageType;
using transport::kFlagNone;
using transport::protocol::encodeExtra;
using transport::protocol::normalizeRelPath;

static int g_failed = 0;

#define CHECK(cond, msg)                       \
    do                                         \
    {                                          \
        if (!(cond))                           \
        {                                      \
            std::printf("FAIL: %s\n", msg);    \
            ++g_failed;                        \
        }                                      \
        else                                   \
        {                                      \
            std::printf("ok: %s\n", msg);      \
        }                                      \
    } while (0)

void testRoundTrip()
{
    const std::string extra = encodeExtra("secret", "docs/a.bin");
    const std::string frame =
        Codec::encodeFrame(MessageType::kUploadReq, extra, nullptr, 0, 1024, 4096, kFlagNone);

    Buffer buf;
    buf.append(frame.data(), frame.size());

    DecodedFrame out;
    CHECK(Codec::tryDecode(&buf, &out) == DecodeStatus::kOk, "full frame decode");
    CHECK(out.header.offset == 1024, "offset");
    CHECK(out.header.fileSize == 4096, "fileSize");
    CHECK(out.token == "secret", "token");
    CHECK(out.path == "docs/a.bin", "path");
    CHECK(buf.readableBytes() == 0, "buffer drained");
}

void testHalfPacket()
{
    const std::string frame =
        Codec::encodeFrame(MessageType::kDownloadReq, "docs/a.bin", nullptr, 0, 0, 0);
    Buffer buf;
    buf.append(frame.data(), frame.size() / 2);

    DecodedFrame out;
    CHECK(Codec::tryDecode(&buf, &out) == DecodeStatus::kNeedMore, "half header/body need more");
    CHECK(buf.readableBytes() == frame.size() / 2, "half packet not consumed");

    buf.append(frame.data() + frame.size() / 2, frame.size() - frame.size() / 2);
    CHECK(Codec::tryDecode(&buf, &out) == DecodeStatus::kOk, "complete after more bytes");
}

void testBadMagic()
{
    const std::string frame =
        Codec::encodeFrame(MessageType::kStatReq, "docs/a.bin", nullptr, 0, 0, 0);
    std::string bad = frame;
    bad[0] = 'X';

    Buffer buf;
    buf.append(bad.data(), bad.size());
    DecodedFrame out;
    CHECK(Codec::tryDecode(&buf, &out) == DecodeStatus::kError, "bad magic");
}

void testBadChecksum()
{
    std::string frame =
        Codec::encodeFrame(MessageType::kUploadDone, "docs/a.bin", nullptr, 0, 0, 8);
    // 篡改 extra 中的路径字节，破坏 checksum（header 40 字节之后）
    CHECK(frame.size() > 41, "frame has extra");
    frame[41] ^= 0x1;

    Buffer buf;
    buf.append(frame.data(), frame.size());
    DecodedFrame out;
    CHECK(Codec::tryDecode(&buf, &out) == DecodeStatus::kError, "bad checksum");
}

void testPathNormalize()
{
    std::string out;
    CHECK(normalizeRelPath("a/b/c", &out) && out == "a/b/c", "normal path");
    CHECK(normalizeRelPath("a/./b", &out) && out == "a/b", "dot segment");
    CHECK(!normalizeRelPath("../x", &out), "reject parent");
    CHECK(!normalizeRelPath("a/../b", &out), "reject parent mid");
    CHECK(!normalizeRelPath("/abs", &out), "reject absolute");
    CHECK(!normalizeRelPath("a\\b", &out), "reject backslash");
    CHECK(!normalizeRelPath(std::string("a\0b", 3), &out), "reject embedded NUL");
    CHECK(transport::protocol::crc32("123456789", 9) == 0xCBF43926u,
          "standard CRC32 vector");
    const uint32_t first = transport::protocol::crc32("1234", 4);
    CHECK(transport::protocol::crc32Append(first, "56789", 5) == 0xCBF43926u,
          "incremental CRC32 vector");
}

void testErrorBody()
{
    const char *msg = "boom";
    const std::string frame =
        Codec::encodeFrame(MessageType::kError, "", msg, std::strlen(msg), 5, 0);
    Buffer buf;
    buf.append(frame.data(), frame.size());
    DecodedFrame out;
    CHECK(Codec::tryDecode(&buf, &out) == DecodeStatus::kOk, "error frame");
    CHECK(out.body == "boom", "error body");
    CHECK(out.header.offset == 5, "error code in offset");
}

int main()
{
    testRoundTrip();
    testHalfPacket();
    testBadMagic();
    testBadChecksum();
    testPathNormalize();
    testErrorBody();

    std::printf("result: %s (%d failed)\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
