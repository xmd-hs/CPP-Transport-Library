#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "Protocol.h"

namespace transport
{

struct TransferProgress
{
    std::string path;
    uint64_t transferred = 0;
    uint64_t total = 0;
    bool upload = false;
};

using TransferProgressCallback = std::function<void(const TransferProgress &)>;
using TransferDoneCallback =
    std::function<void(bool ok, TransferErrorCode code, const std::string &message)>;

} // namespace transport
