#include "SdkInternal.h"
#include "SdkSignalGuard.h"

namespace cldt_sdk
{
bool cancelled(const cldt_client *client)
{
    return client->cancelRequested.load(std::memory_order_acquire);
}

void reportProgress(cldt_client *client, uint64_t transferred, uint64_t total, bool upload)
{
    if (client->progress)
    {
        client->progress(client->userData, transferred, total, upload ? 1 : 0);
    }
}

void reportDone(cldt_client *client, int status, uint64_t total, const std::string &message)
{
    if (client->done)
    {
        client->done(client->userData, status, total, message.c_str());
    }
}

struct Result
{
    int status = CLDT_STATUS_INTERNAL_ERROR;
    uint64_t total = 0;
    std::string message;
};

Result processRequest(cldt_client *client, const Request &request)
{
    Result result;
    if (cancelled(client))
    {
        result.status = CLDT_STATUS_CANCELLED;
        result.message = "cancelled";
        return result;
    }
    Stream socket(client);
    if (!connectStream(socket))
    {
        result.status = socket.error;
        result.message = "DNS/connect/TLS failed";
    }
    else if (request.operation == Operation::kUpload)
        result.status = processUpload(client, socket, request, &result.total, &result.message);
    else if (request.operation == Operation::kDownload)
        result.status = processDownload(client, socket, request, &result.total, &result.message);
    else
        result.status = processStat(socket, client, request, &result.total, &result.message);
    if (socket.error != CLDT_STATUS_OK)
        result.status = socket.error;
    if (cancelled(client))
    {
        result.status = CLDT_STATUS_CANCELLED;
        result.message = "cancelled";
    }
    return result;
}

void workerLoop(cldt_client *client)
{
    WorkerSignalGuard signalGuard;
    (void)signalGuard;
    while (true)
    {
        Request request;
        {
            std::unique_lock<std::mutex> lock(client->mutex);
            client->cv.wait(lock, [client] { return client->stopping || !client->requests.empty(); });
            if (client->stopping && client->requests.empty())
            {
                return;
            }
            request = std::move(client->requests.front());
            client->requests.pop_front();
        }
        Result result;
        try
        {
            result = processRequest(client, request);
        }
        catch (...)
        {
            result.status = CLDT_STATUS_INTERNAL_ERROR;
            result.message = "unexpected SDK exception";
        }
        {
            std::lock_guard<std::mutex> lock(client->mutex);
            client->busy = false;
            client->cancelRequested.store(false, std::memory_order_release);
        }
        // A throwing C++ callback must not terminate the process/worker.
        try { reportDone(client, result.status, result.total, result.message); }
        catch (...) {}
    }
}

int enqueue(cldt_client *client, Request request)
{
    if (!client)
    {
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    std::string normalized;
    if (!transport::protocol::normalizeRelPath(request.remotePath, &normalized) ||
        (request.operation != Operation::kStat && request.localIdentifier.empty()) ||
        request.offset > static_cast<uint64_t>(INT64_MAX) ||
        request.length > static_cast<uint64_t>(INT64_MAX) - request.offset)
        return CLDT_STATUS_INVALID_ARGUMENT;
    request.remotePath.swap(normalized);
    std::lock_guard<std::mutex> lock(client->mutex);
    if (client->stopping)
    {
        return CLDT_STATUS_INTERNAL_ERROR;
    }
    if (client->busy)
    {
        return CLDT_STATUS_BUSY;
    }
    if (request.operation == Operation::kDownload && client->customFileIo &&
        client->durability == CLDT_DURABILITY_SYNC && !client->fileSync)
        return CLDT_STATUS_NOT_SUPPORTED;
    client->busy = true;
    client->cancelRequested.store(false, std::memory_order_release);
    try
    {
        client->requests.emplace_back(std::move(request));
    }
    catch (...)
    {
        client->busy = false;
        return CLDT_STATUS_INTERNAL_ERROR;
    }
    client->cv.notify_one();
    return CLDT_STATUS_OK;
}


} // namespace cldt_sdk
