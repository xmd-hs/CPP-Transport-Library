#include "Protocol.h"

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

namespace transport
{
namespace protocol
{

bool resolveUnderRoot(const std::string &rootDir,
                      const std::string &relPath,
                      std::string *absOut)
{
    if (!absOut)
    {
        return false;
    }
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
