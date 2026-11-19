#include "SdkInternal.h"

#if !defined(_WIN32)
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

namespace cldt_sdk
{
bool sendAll(NativeSocket socket, const void *buffer, size_t length)
{
    const char *data = static_cast<const char *>(buffer);
    while (length > 0)
    {
#if defined(_WIN32)
        const int amount = static_cast<int>(std::min<size_t>(length, 1024 * 1024));
        const int n = ::send(socket, data, amount, 0);
        if (n == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEINTR)
            {
                continue;
            }
            return false;
        }
#else
        const ssize_t n = ::send(socket, data, length, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
#endif
        if (n == 0)
        {
            return false;
        }
        data += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

bool receiveExact(NativeSocket socket, void *buffer, size_t length)
{
    char *data = static_cast<char *>(buffer);
    while (length > 0)
    {
#if defined(_WIN32)
        const int amount = static_cast<int>(std::min<size_t>(length, 1024 * 1024));
        const int n = ::recv(socket, data, amount, 0);
        if (n == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEINTR)
            {
                continue;
            }
            return false;
        }
#else
        const ssize_t n = ::recv(socket, data, length, 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
#endif
        if (n == 0)
        {
            return false;
        }
        data += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

NativeSocket connectSocket(const std::string &host, uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *addresses = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0)
    {
        return kInvalidSocket;
    }

    NativeSocket connected = kInvalidSocket;
    for (addrinfo *address = addresses; address; address = address->ai_next)
    {
        NativeSocket candidate = ::socket(address->ai_family, address->ai_socktype,
                                          address->ai_protocol);
        if (candidate == kInvalidSocket)
        {
            continue;
        }
        if (::connect(candidate, address->ai_addr,
#if defined(_WIN32)
                      static_cast<int>(address->ai_addrlen)
#else
                      static_cast<socklen_t>(address->ai_addrlen)
#endif
                      ) == 0)
        {
            connected = candidate;
            break;
        }
#if defined(_WIN32)
        ::closesocket(candidate);
#else
        ::close(candidate);
#endif
    }
    ::freeaddrinfo(addresses);
    if (connected != kInvalidSocket)
    {
        int one = 1;
        ::setsockopt(connected, IPPROTO_TCP, TCP_NODELAY,
#if defined(_WIN32)
                     reinterpret_cast<const char *>(&one), sizeof(one)
#else
                     &one, sizeof(one)
#endif
        );
#if defined(SO_NOSIGPIPE)
        ::setsockopt(connected, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#if defined(_WIN32)
        DWORD timeout = 60000;
        ::setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        ::setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
        timeval timeout{};
        timeout.tv_sec = 60;
        ::setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    }
    return connected;
}

void closeSocket(NativeSocket socket)
{
    if (socket == kInvalidSocket)
    {
        return;
    }
#if defined(_WIN32)
    ::shutdown(socket, SD_BOTH);
    ::closesocket(socket);
#else
    ::shutdown(socket, SHUT_RDWR);
    ::close(socket);
#endif
}


} // namespace cldt_sdk
