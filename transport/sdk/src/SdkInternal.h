#pragma once
#include "cldt_client_sdk.h"

#include "Protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <cerrno>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <memory>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/types.h>
#endif

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

#ifndef CLDT_SDK_IO_BUFFER_SIZE
#define CLDT_SDK_IO_BUFFER_SIZE (256 * 1024)
#endif

namespace cldt_sdk
{
using Clock = std::chrono::steady_clock;
#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

enum class Operation
{
    kUpload,
    kDownload,
    kStat
};

struct Request
{
    Operation operation;
    std::string localIdentifier;
    std::string remotePath;
    uint64_t offset = 0;
    uint64_t length = 0;
};

struct Frame
{
    uint8_t type = 0;
    uint16_t flags = 0;
    uint64_t bodyLen = 0;
    uint64_t offset = 0;
    uint64_t fileSize = 0;
    std::string extra;
    std::string body;
};

struct Stream
{
    cldt_client *client;
    NativeSocket fd = kInvalidSocket;
    void *tls = nullptr;
    void *tlsContext = nullptr;
    int error = CLDT_STATUS_OK;
    explicit Stream(cldt_client *owner) : client(owner) {}
    ~Stream();
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
};
bool connectStream(Stream &stream);
bool startTls(Stream &stream, Clock::time_point deadline);
void freeTls(Stream &stream);
int tlsRead(Stream &stream, void *data, int size);
int tlsWrite(Stream &stream, const void *data, int size);
bool waitSocket(Stream &stream, bool writing, Clock::time_point deadline);
int socketError();
bool wouldBlock(int error);
void shutdownSocket(NativeSocket fd);

bool sendAll(Stream &socket, const void *buffer, size_t length);
bool receiveExact(Stream &socket, void *buffer, size_t length);
void closeSocket(NativeSocket socket);
bool sendFrame(Stream &socket, uint8_t type, const std::string &extra, const std::string &body, uint64_t bodyLen, uint64_t offset, uint64_t fileSize, uint16_t flags = 0);
bool receiveFrame(Stream &socket, Frame *frame);
// Returns false when the native fast path is unavailable; resume via readAt at done.
bool uploadNativeFile(cldt_client *client, Stream &socket, void *file,
                      uint64_t offset, uint64_t total, uint64_t *done, int *status);
bool cancelled(const cldt_client *client);
void reportProgress(cldt_client *client, uint64_t transferred, uint64_t total, bool upload);
int openFile(cldt_client *client,
             const std::string &identifier,
             uint32_t flags,
             uint64_t expectedSize,
             void **handle,
             uint64_t *size);
bool closeFile(cldt_client *client, void *handle);
bool syncFile(cldt_client *client, void *handle);
bool readAt(cldt_client *client,
            void *handle,
            uint64_t offset,
            void *buffer,
            size_t capacity,
            size_t *bytesRead);
bool writeAt(cldt_client *client,
             void *handle,
             uint64_t offset,
             const void *buffer,
             size_t length,
             size_t *bytesWritten);

void workerLoop(cldt_client *client);
int enqueue(cldt_client *client, Request request);
int processUpload(cldt_client *, Stream &, const Request &, uint64_t *, std::string *);
int processDownload(cldt_client *, Stream &, const Request &, uint64_t *, std::string *);
int processStat(Stream &, const cldt_client *, const Request &, uint64_t *, std::string *);
} // namespace cldt_sdk

struct cldt_client
{
    std::string host;
    uint16_t port = 0;
    std::string token;
    cldt_progress_fn progress = nullptr;
    cldt_done_fn done = nullptr;
    void *userData = nullptr;
    cldt_file_io fileIo{};
    bool customFileIo = false;
    cldt_file_sync_fn fileSync = nullptr;
    uint32_t connectTimeoutMs = 10000;
    uint32_t ioTimeoutMs = 60000;
    bool tlsEnabled = false;
    std::string caFile;
    std::string serverName;
    int durability = CLDT_DURABILITY_SYNC;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<cldt_sdk::Request> requests;
    std::thread worker;
    std::mutex socketMutex;
    bool busy = false;
    bool stopping = false;
    std::atomic_bool cancelRequested{false};
    cldt_sdk::NativeSocket activeSocket = cldt_sdk::kInvalidSocket;
};
