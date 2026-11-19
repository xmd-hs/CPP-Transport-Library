#include "SdkInternal.h"

using namespace cldt_sdk;

extern "C" {

cldt_client *cldt_client_create(const char *host, uint16_t port, const char *token,
                                cldt_progress_fn progress, cldt_done_fn done,
                                void *user_data)
{
    if (!host || !*host || port == 0)
    {
        return nullptr;
    }
#if defined(_WIN32)
    WSADATA data;
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        return nullptr;
    }
#endif
    auto *client = new (std::nothrow) cldt_client();
    if (!client)
    {
#if defined(_WIN32)
        ::WSACleanup();
#endif
        return nullptr;
    }
    client->host = host;
    client->port = port;
    client->token = token ? token : "";
    client->progress = progress;
    client->done = done;
    client->userData = user_data;
    client->fileIo.struct_size = sizeof(cldt_file_io);
    try
    {
        client->worker = std::thread(workerLoop, client);
    }
    catch (...)
    {
        delete client;
#if defined(_WIN32)
        ::WSACleanup();
#endif
        return nullptr;
    }
    return client;
}

int cldt_client_set_file_io(cldt_client *client, const cldt_file_io *file_io)
{
    if (!client)
    {
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(client->mutex);
    if (client->busy || client->stopping)
    {
        return CLDT_STATUS_BUSY;
    }
    if (!file_io)
    {
        client->customFileIo = false;
        client->fileIo = cldt_file_io{};
        return CLDT_STATUS_OK;
    }
    if (file_io->struct_size < sizeof(cldt_file_io) || !file_io->open ||
        !file_io->read_at || !file_io->write_at || !file_io->close)
    {
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    client->fileIo = *file_io;
    client->customFileIo = true;
    return CLDT_STATUS_OK;
}

int cldt_client_upload(cldt_client *client, const char *local_identifier,
                       const char *remote_relative_path, uint64_t offset)
{
    if (!local_identifier || !remote_relative_path)
    {
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    Request request;
    request.operation = Operation::kUpload;
    request.localIdentifier = local_identifier;
    request.remotePath = remote_relative_path;
    request.offset = offset;
    return enqueue(client, std::move(request));
}

int cldt_client_download(cldt_client *client, const char *remote_relative_path,
                         const char *local_identifier, uint64_t offset, uint64_t length)
{
    if (!remote_relative_path || !local_identifier)
    {
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    Request request;
    request.operation = Operation::kDownload;
    request.localIdentifier = local_identifier;
    request.remotePath = remote_relative_path;
    request.offset = offset;
    request.length = length;
    return enqueue(client, std::move(request));
}

int cldt_client_stat(cldt_client *client, const char *remote_relative_path)
{
    if (!remote_relative_path)
    {
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    Request request;
    request.operation = Operation::kStat;
    request.remotePath = remote_relative_path;
    return enqueue(client, std::move(request));
}

int cldt_client_cancel(cldt_client *client)
{
    if (!client)
    {
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    client->cancelRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(client->socketMutex);
        const NativeSocket socket = client->activeSocket;
        if (socket == kInvalidSocket)
        {
            return CLDT_STATUS_OK;
        }
#if defined(_WIN32)
        ::shutdown(socket, SD_BOTH);
#else
        ::shutdown(socket, SHUT_RDWR);
#endif
    }
    return CLDT_STATUS_OK;
}

void cldt_client_destroy(cldt_client *client)
{
    if (!client)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->stopping = true;
        client->cancelRequested.store(true, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(client->socketMutex);
        const NativeSocket socket = client->activeSocket;
        if (socket != kInvalidSocket)
        {
#if defined(_WIN32)
            ::shutdown(socket, SD_BOTH);
#else
            ::shutdown(socket, SHUT_RDWR);
#endif
        }
    }
    client->cv.notify_all();
    if (client->worker.joinable())
    {
        client->worker.join();
    }
    delete client;
#if defined(_WIN32)
    ::WSACleanup();
#endif
}

} // extern "C"
