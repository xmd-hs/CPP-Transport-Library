#include "SdkInternal.h"
#if !defined(_WIN32)
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace cldt_sdk
{
int socketError()
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}
bool wouldBlock(int error)
{
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
}
bool interrupted(int error)
{
#if defined(_WIN32)
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}
void shutdownSocket(NativeSocket fd)
{
    if (fd == kInvalidSocket) return;
#if defined(_WIN32)
    ::shutdown(fd, SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}
void closeSocket(NativeSocket fd)
{
    if (fd == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}
Stream::~Stream()
{
    freeTls(*this);
    std::lock_guard<std::mutex> lock(client->socketMutex);
    if (client->activeSocket == fd) client->activeSocket = kInvalidSocket;
    closeSocket(fd);
}
bool waitSocket(Stream &stream, bool writing, Clock::time_point deadline)
{
    while (!cancelled(stream.client))
    {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0)
        {
            stream.error = CLDT_STATUS_TIMEOUT;
            return false;
        }
        const int timeout = static_cast<int>(std::min<int64_t>(remaining, 50));
#if defined(_WIN32)
        WSAPOLLFD item{};
#else
        pollfd item{};
#endif
        item.fd = stream.fd;
        item.events = writing ? POLLOUT : POLLIN;
#if defined(_WIN32)
        const int result = WSAPoll(&item, 1, timeout);
#else
        const int result = ::poll(&item, 1, timeout);
#endif
        if (result > 0) return true; // Caller checks connect/SO_ERROR or performs I/O.
        if (result < 0 && !interrupted(socketError()))
        {
            stream.error = CLDT_STATUS_NETWORK_ERROR;
            return false;
        }
    }
    stream.error = CLDT_STATUS_CANCELLED;
    return false;
}

// Detached lookups own their state and never access the client after cancel.
// Bound outstanding system resolver calls: repeated cancelled DNS requests
// cannot create an unbounded number of threads. Numeric hosts bypass this path.
struct Addresses
{
    addrinfo *value = nullptr;
#if defined(_WIN32)
    bool winsock = false;
#endif
    ~Addresses()
    {
        if (value) ::freeaddrinfo(value);
#if defined(_WIN32)
        if (winsock) WSACleanup();
#endif
    }
};
struct Lookup
{
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    std::shared_ptr<Addresses> addresses = std::make_shared<Addresses>();
};
std::shared_ptr<Addresses> resolve(Stream &stream, Clock::time_point deadline)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;
    const std::string service = std::to_string(stream.client->port);
    auto numeric = std::make_shared<Addresses>();
    if (::getaddrinfo(stream.client->host.c_str(), service.c_str(), &hints, &numeric->value) == 0)
        return numeric;
    static std::atomic<unsigned> pending{0};
    auto state = std::make_shared<Lookup>();
    const std::string host = stream.client->host;
    if (pending.fetch_add(1) >= 8)
    {
        pending.fetch_sub(1);
        stream.error = CLDT_STATUS_BUSY;
        return nullptr;
    }
    try
    {
        std::thread([state, host, service] {
#if defined(_WIN32)
            WSADATA data;
            const bool initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
            state->addresses->winsock = initialized;
#endif
            addrinfo query{};
            query.ai_family = AF_UNSPEC;
            query.ai_socktype = SOCK_STREAM;
            query.ai_protocol = IPPROTO_TCP;
#if defined(_WIN32)
            if (initialized)
#endif
                ::getaddrinfo(host.c_str(), service.c_str(), &query, &state->addresses->value);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->ready = true;
            }
            state->cv.notify_one();
            // The returned address owner keeps its Winsock reference even when
            // the originating client has already timed out and been destroyed.
            pending.fetch_sub(1);
        }).detach();
    }
    catch (...)
    {
        pending.fetch_sub(1);
        stream.error = CLDT_STATUS_INTERNAL_ERROR;
        return nullptr;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    while (!state->ready)
    {
        if (cancelled(stream.client)) { stream.error = CLDT_STATUS_CANCELLED; return nullptr; }
        if (Clock::now() >= deadline) { stream.error = CLDT_STATUS_TIMEOUT; return nullptr; }
        state->cv.wait_for(lock, std::chrono::milliseconds(25));
    }
    return state->addresses;
}

bool connectStream(Stream &stream)
{
    auto deadline = Clock::now() + std::chrono::milliseconds(stream.client->connectTimeoutMs);
    auto addresses = resolve(stream, deadline);
    if (!addresses || !addresses->value)
    {
        if (stream.error == CLDT_STATUS_OK) stream.error = CLDT_STATUS_CONNECT_FAILED;
        return false;
    }
    for (auto *address = addresses->value; address; address = address->ai_next)
    {
        if (cancelled(stream.client)) { stream.error = CLDT_STATUS_CANCELLED; return false; }
        if (Clock::now() >= deadline) { stream.error = CLDT_STATUS_TIMEOUT; return false; }
        NativeSocket candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == kInvalidSocket) continue;
#if defined(_WIN32)
        u_long nonblocking = 1;
        const bool configured = ioctlsocket(candidate, FIONBIO, &nonblocking) == 0;
#else
        const int flags = fcntl(candidate, F_GETFL, 0);
        const bool configured = flags >= 0 && fcntl(candidate, F_SETFL, flags | O_NONBLOCK) == 0;
        fcntl(candidate, F_SETFD, FD_CLOEXEC);
#endif
        if (!configured) { closeSocket(candidate); continue; }
        stream.fd = candidate;
        {
            std::lock_guard<std::mutex> lock(stream.client->socketMutex);
            stream.client->activeSocket = candidate;
        }
        int result = ::connect(candidate, address->ai_addr,
#if defined(_WIN32)
                               static_cast<int>(address->ai_addrlen));
#else
                               address->ai_addrlen);
#endif
        bool connected = result == 0;
        if (!connected && wouldBlock(socketError()) && waitSocket(stream, true, deadline))
        {
            int error = 0;
#if defined(_WIN32)
            int length = sizeof(error);
            connected = getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                                   reinterpret_cast<char *>(&error), &length) == 0 && error == 0;
#else
            socklen_t length = sizeof(error);
            connected = getsockopt(candidate, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
#endif
        }
        if (connected)
        {
            int one = 1;
            setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY,
#if defined(_WIN32)
                       reinterpret_cast<const char *>(&one), sizeof(one));
#else
                       &one, sizeof(one));
#endif
#if defined(SO_NOSIGPIPE)
            setsockopt(candidate, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            stream.error = CLDT_STATUS_OK;
            return !stream.client->tlsEnabled || startTls(stream, deadline);
        }
        {
            std::lock_guard<std::mutex> lock(stream.client->socketMutex);
            stream.client->activeSocket = kInvalidSocket;
            closeSocket(candidate);
            stream.fd = kInvalidSocket;
        }
        if (stream.error == CLDT_STATUS_CANCELLED || stream.error == CLDT_STATUS_TIMEOUT) return false;
    }
    stream.error = CLDT_STATUS_CONNECT_FAILED;
    return false;
}

bool transfer(Stream &stream, void *buffer, size_t length, bool writing)
{
    char *data = static_cast<char *>(buffer);
    auto deadline = Clock::now() + std::chrono::milliseconds(stream.client->ioTimeoutMs);
    while (length)
    {
        if (cancelled(stream.client)) { stream.error = CLDT_STATUS_CANCELLED; return false; }
        const int amount = static_cast<int>(std::min<size_t>(length, 1024 * 1024));
        int n;
        if (stream.tls)
        {
            n = writing ? tlsWrite(stream, data, amount) : tlsRead(stream, data, amount);
            if (n == -2 || n == -3)
            {
                if (!waitSocket(stream, n == -3, deadline)) return false;
                continue;
            }
        }
        else
        {
            n = static_cast<int>(writing ? ::send(stream.fd, data, amount, MSG_NOSIGNAL)
                                         : ::recv(stream.fd, data, amount, 0));
            if (n < 0)
            {
                const int error = socketError();
                if (interrupted(error)) continue;
                if (wouldBlock(error))
                {
                    if (!waitSocket(stream, writing, deadline)) return false;
                    continue;
                }
            }
        }
        if (n <= 0)
        {
            if (stream.error == CLDT_STATUS_OK) stream.error = CLDT_STATUS_NETWORK_ERROR;
            return false;
        }
        data += n;
        length -= static_cast<size_t>(n);
        deadline = Clock::now() + std::chrono::milliseconds(stream.client->ioTimeoutMs);
    }
    return true;
}
bool sendAll(Stream &stream, const void *buffer, size_t length)
{
    return transfer(stream, const_cast<void *>(buffer), length, true);
}
bool receiveExact(Stream &stream, void *buffer, size_t length)
{
    return transfer(stream, buffer, length, false);
}
} // namespace cldt_sdk
