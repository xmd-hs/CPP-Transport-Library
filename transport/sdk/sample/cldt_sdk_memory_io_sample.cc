#include "cldt_client_sdk.h"
#include "sample_argv_utf8.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace
{

struct MemoryFiles
{
    std::vector<uint8_t> source;
    std::vector<uint8_t> destination;
};

struct MemoryHandle
{
    MemoryFiles *files;
    bool writing;
};

struct Completion
{
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    int status = CLDT_STATUS_INTERNAL_ERROR;
    uint64_t total = 0;
    std::string message;
};

int openMemoryFile(void *context, const char *identifier, uint32_t flags,
                   uint64_t expectedSize, void **handle, uint64_t *fileSize)
{
    auto *files = static_cast<MemoryFiles *>(context);
    bool writing = false;
    if (std::strcmp(identifier, "memory:source") == 0 && (flags & CLDT_FILE_READ))
    {
        *fileSize = files->source.size();
    }
    else if (std::strcmp(identifier, "memory:destination") == 0 &&
             (flags & CLDT_FILE_WRITE))
    {
        writing = true;
        if (flags & CLDT_FILE_TRUNCATE)
        {
            if (expectedSize > std::numeric_limits<size_t>::max())
            {
                return -1;
            }
            files->destination.assign(static_cast<size_t>(expectedSize), 0);
        }
        *fileSize = files->destination.size();
    }
    else
    {
        return -1;
    }

    *handle = new (std::nothrow) MemoryHandle{files, writing};
    return *handle ? 0 : -1;
}

int readMemoryFile(void *opaque, uint64_t offset, void *buffer, size_t capacity,
                   size_t *bytesRead)
{
    auto *handle = static_cast<MemoryHandle *>(opaque);
    if (handle->writing || offset > handle->files->source.size())
    {
        return -1;
    }
    const size_t position = static_cast<size_t>(offset);
    const size_t amount = std::min(capacity, handle->files->source.size() - position);
    if (amount)
    {
        std::memcpy(buffer, handle->files->source.data() + position, amount);
    }
    *bytesRead = amount;
    return 0;
}

int writeMemoryFile(void *opaque, uint64_t offset, const void *buffer, size_t length,
                    size_t *bytesWritten)
{
    auto *handle = static_cast<MemoryHandle *>(opaque);
    if (!handle->writing || offset > std::numeric_limits<size_t>::max() ||
        length > std::numeric_limits<size_t>::max() - static_cast<size_t>(offset))
    {
        return -1;
    }
    auto &destination = handle->files->destination;
    const size_t end = static_cast<size_t>(offset) + length;
    if (destination.size() < end)
    {
        destination.resize(end);
    }
    if (length)
    {
        std::memcpy(destination.data() + static_cast<size_t>(offset), buffer, length);
    }
    *bytesWritten = length;
    return 0;
}

void closeMemoryFile(void *opaque)
{
    delete static_cast<MemoryHandle *>(opaque);
}

void onDone(void *opaque, int status, uint64_t total, const char *message)
{
    auto *completion = static_cast<Completion *>(opaque);
    {
        std::lock_guard<std::mutex> lock(completion->mutex);
        completion->ready = true;
        completion->status = status;
        completion->total = total;
        completion->message = message ? message : "";
    }
    completion->cv.notify_one();
}

bool waitFor(cldt_client *client, Completion *completion, int submitStatus)
{
    if (submitStatus != CLDT_STATUS_OK)
    {
        std::cerr << "request rejected: " << submitStatus << '\n';
        return false;
    }
    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->cv.wait_for(lock, std::chrono::seconds(30), [completion] {
            return completion->ready;
        }))
    {
        lock.unlock();
        cldt_client_cancel(client);
        std::cerr << "request timed out\n";
        return false;
    }
    const bool ok = completion->status == CLDT_STATUS_OK;
    if (!ok)
    {
        std::cerr << "request failed: " << completion->status << ' '
                  << completion->message << '\n';
    }
    completion->ready = false;
    return ok;
}

} // namespace

int runSample(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: " << argv[0] << " <host> <port> <token> [remote-path]\n";
        return 1;
    }
    const unsigned long port = std::strtoul(argv[2], nullptr, 10);
    if (port == 0 || port > 65535)
    {
        return 1;
    }

    MemoryFiles files;
    files.source.resize(3 * 1024 * 1024 + 137);
    for (size_t i = 0; i < files.source.size(); ++i)
    {
        files.source[i] = static_cast<uint8_t>((i * 31 + 7) % 251);
    }

    Completion completion;
    cldt_client *client = cldt_client_create(argv[1], static_cast<uint16_t>(port),
                                              argv[3], nullptr, onDone, &completion);
    if (!client)
    {
        std::cerr << "failed to create SDK client\n";
        return 1;
    }
    // This example stores bytes in RAM and cannot promise durable storage.
    cldt_client_options options;
    cldt_client_options_init(&options);
    options.download_durability = CLDT_DURABILITY_BUFFERED;
    if (cldt_client_set_options(client, &options) != CLDT_STATUS_OK)
    {
        cldt_client_destroy(client);
        return 1;
    }
    cldt_file_io fileIo{};
    fileIo.struct_size = sizeof(fileIo);
    fileIo.context = &files;
    fileIo.open = openMemoryFile;
    fileIo.read_at = readMemoryFile;
    fileIo.write_at = writeMemoryFile;
    fileIo.close = closeMemoryFile;
    if (cldt_client_set_file_io(client, &fileIo) != CLDT_STATUS_OK)
    {
        cldt_client_destroy(client);
        return 1;
    }

    const std::string remotePath = argc >= 5 ? argv[4] : "sdk/memory.bin";
    bool ok = waitFor(client, &completion,
                      cldt_client_upload(client, "memory:source", remotePath.c_str(), 0));
    ok = waitFor(client, &completion,
                 ok ? cldt_client_stat(client, remotePath.c_str())
                    : CLDT_STATUS_INTERNAL_ERROR) && ok;
    if (completion.total != files.source.size())
    {
        ok = false;
    }
    ok = waitFor(client, &completion,
                 ok ? cldt_client_download(client, remotePath.c_str(),
                                           "memory:destination", 0, 0)
                    : CLDT_STATUS_INTERNAL_ERROR) && ok;
    ok = ok && files.source == files.destination;
    cldt_client_destroy(client);
    if (!ok)
    {
        std::cerr << "custom file I/O round-trip failed\n";
        return 1;
    }
    std::cout << "PASS: custom I/O upload, stat, download, byte comparison ("
              << files.source.size() << " bytes)\n";
    return 0;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t **wideArgv)
{
    std::vector<std::string> utf8Storage;
    std::vector<char *> utf8Argv;
    if (!cldt_sample::makeUtf8Argv(argc, wideArgv, &utf8Storage, &utf8Argv))
    {
        std::cerr << "failed to convert command-line arguments to UTF-8\n";
        return 1;
    }
    return runSample(argc, utf8Argv.data());
}
#else
int main(int argc, char **argv)
{
    return runSample(argc, argv);
}
#endif
