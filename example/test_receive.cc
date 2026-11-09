#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include "Buffer.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "TcpConnection.h"
#include "TimerId.h"

bool run(bool closeInCallback)
{
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) return false;
    ::fcntl(sockets[0], F_SETFL, ::fcntl(sockets[0], F_GETFL) | O_NONBLOCK);
    const std::string source(4 * 1024 * 1024 + 37, 'r');
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(
        &loop, "receive-test", sockets[0], InetAddress(), InetAddress());
    size_t received = 0;
    size_t turnBytes = 0;
    int turnCallbacks = 0;
    int callbacks = 0;
    bool turnScheduled = false;
    bool valid = true;
    bool done = false;
    // Leave a prefix unread in each callback: later input must preserve it and
    // the batching fast path must fall back when framing retains bytes.
    conn->setMessageCallback([&](const TcpConnectionPtr &c, Buffer *b, Timestamp) {
        ++callbacks;
        ++turnCallbacks;
        const size_t available = b->readableBytes();
        const size_t retained = received ? 17 : 0;
        if (available < retained) valid = false;
        turnBytes += available - retained;
        valid = valid && turnCallbacks <= 4 && turnBytes <= 1024 * 1024;
        if (!turnScheduled) {
            turnScheduled = true;
            loop.queueInLoop([&] { turnBytes = 0; turnCallbacks = 0; turnScheduled = false; });
        }
        if (closeInCallback) {
            b->retrieveAll();
            c->connectDestroyed();
            done = true;
            loop.quit();
            return;
        }
        for (size_t i = 0; i < available; ++i) valid = valid && b->peek()[i] == 'r';
        const bool final = received + available == source.size();
        const size_t consume = final ? available : (available > 17 ? available - 17 : 0);
        b->retrieve(consume);
        received += consume;
        if (received == source.size()) { done = true; loop.quit(); }
    });
    conn->setCloseCallback([&](const TcpConnectionPtr &) { loop.quit(); });
    conn->connectEstablished();
    std::thread writer([&] {
        size_t offset = 0;
        while (offset < source.size()) {
            const auto n = ::send(sockets[1], source.data() + offset,
                                  source.size() - offset, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            offset += static_cast<size_t>(n);
        }
    });
    loop.runAfter(3, [&] { loop.quit(); });
    loop.loop();
    if (!closeInCallback || !done) conn->connectDestroyed();
    ::shutdown(sockets[1], SHUT_RDWR);
    writer.join();
    ::close(sockets[1]);
    return valid && done && (closeInCallback ? callbacks == 1 : received == source.size());
}

int main()
{
    const bool retained = run(false);
    const bool closed = run(true);
    std::printf("retained bytes: %s; callback closes connection: %s\n",
                retained ? "PASS" : "FAIL", closed ? "PASS" : "FAIL");
    return retained && closed ? 0 : 1;
}