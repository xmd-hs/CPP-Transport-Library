#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace transport
{

#pragma pack(push, 1)
struct TransferHeader
{
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t headerExtra;
    uint64_t bodyLen;
    uint64_t offset;
    uint64_t fileSize;
    uint32_t checksum; // CRC32(header without checksum + extra + in-frame body)
};
#pragma pack(pop)

static_assert(sizeof(TransferHeader) == 40, "TransferHeader must be 40 bytes");

enum class MessageType : uint8_t
{
    kUploadReq = 1,
    kUploadDone = 2,
    kDownloadReq = 3,
    kDownloadResp = 4,
    kError = 5,
    kStatReq = 6,
    kStatResp = 7,
};

enum TransferFlags : uint16_t
{
    kFlagNone = 0,
    kFlagFinal = 1 << 0,
};

enum class TransferErrorCode : int
{
    kOk = 0,
    kBadMagic = 1,
    kBadVersion = 2,
    kBadPath = 3,
    kNotFound = 4,
    kIOError = 5,
    kProtocol = 6,
    kBusy = 7,
    kUnauthorized = 8,
    kBadChecksum = 9,
};

namespace protocol
{

constexpr uint32_t kMagic = 0x434C4454u; // 'CLDT'
constexpr uint8_t kVersion = 2;
constexpr size_t kHeaderSize = sizeof(TransferHeader);
constexpr size_t kMaxPathLen = 4096;
constexpr size_t kMaxErrorLen = 1024;
constexpr size_t kMaxTokenLen = 256;
constexpr size_t kPumpBatch = 1024 * 1024;
constexpr size_t kPumpChunk = 256 * 1024;

uint32_t crc32(const void *data, size_t len);
uint32_t crc32Append(uint32_t crc, const void *data, size_t len);

inline void encodeHeader(TransferHeader *out,
                         MessageType type,
                         uint32_t headerExtra,
                         uint64_t bodyLen,
                         uint64_t offset,
                         uint64_t fileSize,
                         uint16_t flags = kFlagNone)
{
    *out = TransferHeader{};
    out->magic = kMagic;
    out->version = kVersion;
    out->type = static_cast<uint8_t>(type);
    out->flags = flags;
    out->headerExtra = headerExtra;
    out->bodyLen = bodyLen;
    out->offset = offset;
    out->fileSize = fileSize;
    out->checksum = 0;
}

// extra 编码：token 为空则仅 path；否则 token + '\0' + path
std::string encodeExtra(const std::string &token, const std::string &path);
bool decodeExtra(const std::string &extra, std::string *token, std::string *path);

// 规范化相对路径：拒绝绝对路径、..、空段、反斜杠；输出用 '/' 连接。
bool normalizeRelPath(const std::string &input, std::string *normalized);

// 将 root + rel 解析为绝对路径，并确保结果仍落在 root 下。
bool resolveUnderRoot(const std::string &rootDir,
                      const std::string &relPath,
                      std::string *absOut);

} // namespace protocol
} // namespace transport
