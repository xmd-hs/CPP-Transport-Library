#ifndef CLDT_SAMPLE_ARGV_UTF8_H
#define CLDT_SAMPLE_ARGV_UTF8_H

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace cldt_sample
{

inline bool makeUtf8Argv(int argc, wchar_t **wideArgv,
                         std::vector<std::string> *storage,
                         std::vector<char *> *argv)
{
    storage->clear();
    argv->clear();
    storage->reserve(static_cast<size_t>(argc));
    argv->reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
    {
        const int length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                  wideArgv[i], -1, nullptr, 0,
                                                  nullptr, nullptr);
        if (length <= 0)
        {
            return false;
        }
        std::string value(static_cast<size_t>(length), '\0');
        if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArgv[i], -1,
                                  &value[0], length, nullptr, nullptr) <= 0)
        {
            return false;
        }
        value.resize(static_cast<size_t>(length - 1));
        storage->push_back(std::move(value));
    }
    for (std::string &value : *storage)
    {
        argv->push_back(const_cast<char *>(value.c_str()));
    }
    return true;
}

} // namespace cldt_sample
#endif

#endif
