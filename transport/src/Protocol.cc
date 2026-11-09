#include "Protocol.h"

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>
#include <array>

namespace transport
{
namespace protocol
{

namespace
{

const std::array<uint32_t, 256> &crcTable()
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
            {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            values[i] = c;
        }
        return values;
    }();
    return table;
}

uint32_t crcUpdateRaw(uint32_t crc, const void *data, size_t len)
{
    const auto &table = crcTable();
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i)
    {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

} // namespace

uint32_t crc32Append(uint32_t crc, const void *data, size_t len)
{
    // crc 为已 finalize 的值；还原中间态再继续更新。
    uint32_t state = crc ^ 0xFFFFFFFFu;
    state = crcUpdateRaw(state, data, len);
    return state ^ 0xFFFFFFFFu;
}

uint32_t crc32(const void *data, size_t len)
{
    uint32_t state = 0xFFFFFFFFu;
    state = crcUpdateRaw(state, data, len);
    return state ^ 0xFFFFFFFFu;
}

std::string encodeExtra(const std::string &token, const std::string &path)
{
    if (token.empty())
    {
        return path;
    }
    std::string out;
    out.reserve(token.size() + 1 + path.size());
    out.append(token);
    out.push_back('\0');
    out.append(path);
    return out;
}

bool decodeExtra(const std::string &extra, std::string *token, std::string *path)
{
    const auto pos = extra.find('\0');
    if (pos == std::string::npos)
    {
        token->clear();
        *path = extra;
        return true;
    }
    if (pos > kMaxTokenLen)
    {
        return false;
    }
    *token = extra.substr(0, pos);
    *path = extra.substr(pos + 1);
    return true;
}

bool normalizeRelPath(const std::string &input, std::string *normalized)
{
    if (input.empty() || input.size() > kMaxPathLen)
    {
        return false;
    }
    if (input[0] == '/' || input.find('\\') != std::string::npos ||
        input.find('\0') != std::string::npos)
    {
        return false;
    }

    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty() || cur == ".")
        {
            cur.clear();
            return true;
        }
        if (cur == "..")
        {
            return false;
        }
        parts.push_back(cur);
        cur.clear();
        return true;
    };

    for (char ch : input)
    {
        if (ch == '/')
        {
            if (!flush())
            {
                return false;
            }
        }
        else
        {
            cur.push_back(ch);
        }
    }
    if (!flush())
    {
        return false;
    }
    if (parts.empty())
    {
        return false;
    }

    normalized->clear();
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
        {
            normalized->push_back('/');
        }
        normalized->append(parts[i]);
    }
    return true;
}

bool resolveUnderRoot(const std::string &rootDir,
                      const std::string &relPath,
                      std::string *absOut)
{
    std::string normalized;
    if (!normalizeRelPath(relPath, &normalized))
    {
        return false;
    }

    std::string root = rootDir.empty() ? "." : rootDir;
    char rootReal[PATH_MAX];
    if (::realpath(root.c_str(), rootReal) == nullptr)
    {
        std::string joined = root;
        if (joined.back() != '/')
        {
            joined.push_back('/');
        }
        joined.append(normalized);
        *absOut = joined;
        return true;
    }

    std::string joined = rootReal;
    if (joined.back() != '/')
    {
        joined.push_back('/');
    }
    joined.append(normalized);

    std::string probe = joined;
    while (!probe.empty())
    {
        char realBuf[PATH_MAX];
        if (::realpath(probe.c_str(), realBuf) != nullptr)
        {
            const std::string real(realBuf);
            const std::string rootPrefix(rootReal);
            if (real.size() < rootPrefix.size() ||
                real.compare(0, rootPrefix.size(), rootPrefix) != 0)
            {
                return false;
            }
            if (real.size() > rootPrefix.size() && rootPrefix.back() != '/' &&
                real[rootPrefix.size()] != '/')
            {
                return false;
            }
            break;
        }
        const auto slash = probe.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
        {
            break;
        }
        probe.resize(slash);
    }

    *absOut = joined;
    return true;
}

} // namespace protocol
} // namespace transport
