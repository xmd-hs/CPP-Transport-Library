#include "SdkInternal.h"

namespace cldt_sdk
{
bool sendDownloadRequest(NativeSocket socket, const cldt_client *client, const Request &request)
{
    std::string normalized;
    if (!transport::protocol::normalizeRelPath(request.remotePath, &normalized))
    {
        return false;
    }
    const std::string extra = transport::protocol::encodeExtra(client->token, normalized);
    return sendFrame(socket, static_cast<uint8_t>(transport::MessageType::kDownloadReq),
                     extra, std::string(), request.length, request.offset, 0);
}

int processUpload(cldt_client *client, NativeSocket socket, const Request &request,
                  uint64_t *total, std::string *message)
{
    void *file = nullptr;
    uint64_t fileSize = 0;
    if (openFile(client, request.localIdentifier, CLDT_FILE_READ, 0, &file, &fileSize) != 0 ||
        request.offset > fileSize)
    {
        closeFile(client, file);
        *message = "cannot open source or offset exceeds file size";
        return CLDT_STATUS_FILE_ERROR;
    }

    std::string normalized;
    if (!transport::protocol::normalizeRelPath(request.remotePath, &normalized))
    {
        closeFile(client, file);
        *message = "invalid remote path";
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    if (!sendFrame(socket, static_cast<uint8_t>(transport::MessageType::kUploadReq),
                   transport::protocol::encodeExtra(client->token, normalized),
                   std::string(), 0, request.offset, fileSize))
    {
        closeFile(client, file);
        *message = "failed to send upload request";
        return CLDT_STATUS_NETWORK_ERROR;
    }

    *total = fileSize - request.offset;
    uint64_t done = 0;
    int nativeStatus = CLDT_STATUS_OK;
    const bool nativeHandled = uploadNativeFile(client, socket, file, request.offset,
                                                 *total, &done, &nativeStatus);
    if (nativeHandled && nativeStatus != CLDT_STATUS_OK)
    {
        closeFile(client, file);
        *message = "native upload failed or cancelled";
        return nativeStatus;
    }
    // Native uploads avoid the working buffer and both userspace copies.
    std::vector<char> buffer(done < *total ? CLDT_SDK_IO_BUFFER_SIZE : 0);
    auto lastReport = Clock::now() - std::chrono::seconds(1);
    while (done < *total)
    {
        if (cancelled(client))
        {
            closeFile(client, file);
            *message = "cancelled";
            return CLDT_STATUS_CANCELLED;
        }
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), *total - done));
        size_t got = 0;
        while (got < wanted)
        {
            size_t n = 0;
            if (!readAt(client, file, request.offset + done + got,
                        buffer.data() + got, wanted - got, &n) || n == 0)
            {
                closeFile(client, file);
                *message = "source read failed or reached EOF";
                return CLDT_STATUS_FILE_ERROR;
            }
            got += n;
        }
        if (!sendAll(socket, buffer.data(), got))
        {
            closeFile(client, file);
            *message = "upload stream send failed";
            return cancelled(client) ? CLDT_STATUS_CANCELLED : CLDT_STATUS_NETWORK_ERROR;
        }
        done += got;
        if (Clock::now() - lastReport >= std::chrono::milliseconds(100) || done == *total)
        {
            reportProgress(client, done, *total, true);
            lastReport = Clock::now();
        }
    }
    closeFile(client, file);

    Frame response;
    if (!receiveFrame(socket, &response))
    {
        *message = cancelled(client) ? "cancelled" : "invalid upload response";
        return cancelled(client) ? CLDT_STATUS_CANCELLED : CLDT_STATUS_PROTOCOL_ERROR;
    }
    if (response.type == static_cast<uint8_t>(transport::MessageType::kError))
    {
        *message = response.body;
        return CLDT_STATUS_REMOTE_ERROR;
    }
    if (response.type != static_cast<uint8_t>(transport::MessageType::kUploadDone))
    {
        *message = "unexpected upload response";
        return CLDT_STATUS_PROTOCOL_ERROR;
    }
    *message = "upload done";
    return CLDT_STATUS_OK;
}

int processDownload(cldt_client *client, NativeSocket socket, const Request &request,
                    uint64_t *total, std::string *message)
{
    if (!sendDownloadRequest(socket, client, request))
    {
        *message = "invalid remote path or failed to send request";
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    Frame response;
    if (!receiveFrame(socket, &response))
    {
        *message = "invalid download response";
        return CLDT_STATUS_PROTOCOL_ERROR;
    }
    if (response.type == static_cast<uint8_t>(transport::MessageType::kError))
    {
        *message = response.body;
        return CLDT_STATUS_REMOTE_ERROR;
    }
    if (response.type != static_cast<uint8_t>(transport::MessageType::kDownloadResp) ||
        response.offset != request.offset)
    {
        *message = "unexpected download response";
        return CLDT_STATUS_PROTOCOL_ERROR;
    }

    void *file = nullptr;
    uint64_t ignoredSize = 0;
    const uint32_t flags = CLDT_FILE_WRITE |
                           ((request.offset == 0 && request.length == 0)
                                ? CLDT_FILE_TRUNCATE
                                : 0);
    if (openFile(client, request.localIdentifier, flags, response.fileSize,
                 &file, &ignoredSize) != 0)
    {
        *message = "cannot open destination";
        return CLDT_STATUS_FILE_ERROR;
    }

    *total = response.bodyLen;
    reportProgress(client, 0, *total, false);
    std::vector<char> buffer(CLDT_SDK_IO_BUFFER_SIZE);
    uint64_t done = 0;
    auto lastReport = Clock::now() - std::chrono::seconds(1);
    while (done < *total)
    {
        if (cancelled(client))
        {
            closeFile(client, file);
            *message = "cancelled";
            return CLDT_STATUS_CANCELLED;
        }
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), *total - done));
        if (!receiveExact(socket, buffer.data(), wanted))
        {
            closeFile(client, file);
            *message = cancelled(client) ? "cancelled" : "download stream receive failed";
            return cancelled(client) ? CLDT_STATUS_CANCELLED : CLDT_STATUS_NETWORK_ERROR;
        }
        size_t written = 0;
        while (written < wanted)
        {
            size_t n = 0;
            if (!writeAt(client, file, request.offset + done + written,
                         buffer.data() + written, wanted - written, &n) || n == 0)
            {
                closeFile(client, file);
                *message = "destination write failed";
                return CLDT_STATUS_FILE_ERROR;
            }
            written += n;
        }
        done += wanted;
        if (Clock::now() - lastReport >= std::chrono::milliseconds(100) || done == *total)
        {
            reportProgress(client, done, *total, false);
            lastReport = Clock::now();
        }
    }
    closeFile(client, file);
    *message = "download done";
    return CLDT_STATUS_OK;
}

int processStat(NativeSocket socket, const cldt_client *client, const Request &request,
                uint64_t *total, std::string *message)
{
    std::string normalized;
    if (!transport::protocol::normalizeRelPath(request.remotePath, &normalized) ||
        !sendFrame(socket, static_cast<uint8_t>(transport::MessageType::kStatReq),
                   transport::protocol::encodeExtra(client->token, normalized),
                   std::string(), 0, 0, 0))
    {
        *message = "invalid path or failed to send request";
        return CLDT_STATUS_INVALID_ARGUMENT;
    }
    Frame response;
    if (!receiveFrame(socket, &response))
    {
        *message = "invalid stat response";
        return CLDT_STATUS_PROTOCOL_ERROR;
    }
    if (response.type == static_cast<uint8_t>(transport::MessageType::kError))
    {
        *message = response.body;
        return CLDT_STATUS_REMOTE_ERROR_BASE + static_cast<int>(response.offset);
    }
    if (response.type != static_cast<uint8_t>(transport::MessageType::kStatResp))
    {
        *message = "unexpected stat response";
        return CLDT_STATUS_PROTOCOL_ERROR;
    }
    *total = response.fileSize;
    *message = "stat done";
    return CLDT_STATUS_OK;
}


} // namespace cldt_sdk
