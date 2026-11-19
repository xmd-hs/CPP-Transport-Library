#include "cldt_client_sdk.h"
#include "sample_argv_utf8.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace
{

struct Completion
{
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    int status = CLDT_STATUS_INTERNAL_ERROR;
    uint64_t total = 0;
    std::string message;
};

void onProgress(void *, uint64_t transferred, uint64_t total, int upload)
{
    if (total == 0)
    {
        return;
    }
    std::fprintf(stderr, "\r%s: %llu / %llu bytes",
                 upload ? "upload" : "download",
                 static_cast<unsigned long long>(transferred),
                 static_cast<unsigned long long>(total));
}

void onDone(void *userData, int status, uint64_t total, const char *message)
{
    auto *completion = static_cast<Completion *>(userData);
    {
        std::lock_guard<std::mutex> lock(completion->mutex);
        completion->ready = true;
        completion->status = status;
        completion->total = total;
        completion->message = message ? message : "";
    }
    completion->cv.notify_one();
}

void usage(const char *program)
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s <host> <port> <token> upload <local-file> <remote-path> [offset]\n"
                 "  %s <host> <port> <token> download <remote-path> <local-file> [offset] [length]\n"
                 "  %s <host> <port> <token> stat <remote-path>\n",
                 program, program, program);
}

} // namespace

int runSample(int argc, char **argv)
{
    if (argc < 6)
    {
        usage(argv[0]);
        return 1;
    }
    const auto portValue = std::strtoul(argv[2], nullptr, 10);
    if (portValue == 0 || portValue > 65535)
    {
        usage(argv[0]);
        return 1;
    }

    Completion completion;
    cldt_client *client = cldt_client_create(
        argv[1], static_cast<uint16_t>(portValue), argv[3], onProgress, onDone, &completion);
    if (!client)
    {
        std::fprintf(stderr, "failed to create SDK client\n");
        return 1;
    }

    const std::string command = argv[4];
    int submitStatus = CLDT_STATUS_INVALID_ARGUMENT;
    if (command == "upload" && argc >= 7)
    {
        const uint64_t offset = argc >= 8 ? std::strtoull(argv[7], nullptr, 10) : 0;
        submitStatus = cldt_client_upload(client, argv[5], argv[6], offset);
    }
    else if (command == "download" && argc >= 7)
    {
        const uint64_t offset = argc >= 8 ? std::strtoull(argv[7], nullptr, 10) : 0;
        const uint64_t length = argc >= 9 ? std::strtoull(argv[8], nullptr, 10) : 0;
        submitStatus = cldt_client_download(client, argv[5], argv[6], offset, length);
    }
    else if (command == "stat" && argc >= 6)
    {
        submitStatus = cldt_client_stat(client, argv[5]);
    }
    if (submitStatus != CLDT_STATUS_OK)
    {
        std::fprintf(stderr, "request rejected: %d\n", submitStatus);
        cldt_client_destroy(client);
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(completion.mutex);
        if (!completion.cv.wait_for(lock, std::chrono::minutes(10), [&] {
                return completion.ready;
            }))
        {
            lock.unlock();
            cldt_client_cancel(client);
            cldt_client_destroy(client);
            std::fprintf(stderr, "request timed out\n");
            return 1;
        }
    }
    cldt_client_destroy(client);
    std::fprintf(stderr, "\n%s (%llu bytes)\n", completion.message.c_str(),
                 static_cast<unsigned long long>(completion.total));
    return completion.status == CLDT_STATUS_OK ? 0 : 2;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t **wideArgv)
{
    std::vector<std::string> utf8Storage;
    std::vector<char *> utf8Argv;
    if (!cldt_sample::makeUtf8Argv(argc, wideArgv, &utf8Storage, &utf8Argv))
    {
        std::fprintf(stderr, "failed to convert command-line arguments to UTF-8\n");
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
