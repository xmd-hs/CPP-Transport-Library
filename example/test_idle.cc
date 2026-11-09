#include <cerrno>
#include <cstdio>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

#include "EventLoop.h"
#include "InetAddress.h"
#include "TcpConnection.h"
#include "TransferSession.h"

int main()
{
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) != 0)
        return 1;
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(
        &loop, "idle-test", sockets[0], InetAddress(), InetAddress());
    conn->connectEstablished();
    auto session = std::make_shared<transport::TransferSession>(
        transport::TransferSession::Role::kServer, conn, ".", "", 0.2);
    session->startIdleGuard();
    bool keptAlive = false;
    bool fileActivityKeptAlive = false;
    bool timedOut = false;
    FILE *file = std::tmpfile();
    if (!file) return 1;
    std::fwrite("payload", 1, 7, file);
    std::fflush(file);
    loop.runAfter(0.12, [session] { session->touch(); });
    loop.runAfter(0.25, [&] {
        char byte;
        const auto n = ::recv(sockets[1], &byte, 1, MSG_DONTWAIT);
        keptAlive = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    });
    loop.runAfter(0.27, [&] {
        // No write-complete callback is installed: only actual file I/O should
        // extend the deadline, as happens during a long pending file send.
        conn->sendFile(::fileno(file), 0, 7);
        char bytes[7];
        ::recv(sockets[1], bytes, sizeof(bytes), MSG_DONTWAIT);
    });
    loop.runAfter(0.4, [&] {
        char byte;
        const auto n = ::recv(sockets[1], &byte, 1, MSG_DONTWAIT);
        fileActivityKeptAlive = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    });
    loop.runAfter(0.65, [&] {
        char byte;
        timedOut = ::recv(sockets[1], &byte, 1, MSG_DONTWAIT) == 0;
        loop.quit();
    });
    loop.loop();
    session.reset();
    conn->connectDestroyed();
    conn.reset();
    std::fclose(file);
    ::close(sockets[1]);
    std::printf("activity extends deadline: %s; idle closes socket: %s\n",
                keptAlive ? "PASS" : "FAIL", timedOut ? "PASS" : "FAIL");
    std::printf("file write activity extends deadline: %s\n", fileActivityKeptAlive ? "PASS" : "FAIL");
    return keptAlive && fileActivityKeptAlive && timedOut ? 0 : 1;
}
