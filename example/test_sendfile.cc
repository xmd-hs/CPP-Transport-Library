#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "EventLoop.h"
#include "InetAddress.h"
#include "TcpConnection.h"
#include "TimerId.h"

bool run(bool prematureEof)
{
    char path[] = "/tmp/muduo-sendfile-XXXXXX";
    const int file = ::mkstemp(path);
    if (file < 0) return false;
    ::unlink(path);
    std::string source(2 * 1024 * 1024 + 300, '\0');
    for (size_t i = 0; i < source.size(); ++i) source[i] = char(i % 251);
    size_t written = 0;
    while (written < source.size()) {
        const auto n = ::write(file, source.data() + written, source.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(file); return false; }
        written += size_t(n);
    }
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        ::close(file);
        return false;
    }
    ::fcntl(sockets[0], F_SETFL, O_NONBLOCK);
    EventLoop loop;
    auto conn = std::make_shared<TcpConnection>(
        &loop, "sendfile-test", sockets[0], InetAddress(), InetAddress());
    conn->connectEstablished();
    int small = 8192;
    ::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    bool complete = false, closed = false, timeout = false;
    conn->setWriteCompleteCallback([&](const TcpConnectionPtr &) {
        complete = true;
        loop.quit();
    });
    conn->setCloseCallback([&](const TcpConnectionPtr &) {
        closed = true;
        loop.quit();
    });
    const size_t offset = 137;
    const size_t count = prematureEof ? source.size() : source.size() - 300;
    conn->sendFile(file, offset, count);
    const bool backpressured = conn->isSendingFile();
    ::close(file); // The pending send must survive closing/reusing the caller's fd.
    const int replacement = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    std::string received;
    std::thread reader([&] {
        char buffer[16384];
        while (true) {
            const auto n = ::read(sockets[1], buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            received.append(buffer, size_t(n));
        }
    });
    loop.runAfter(3, [&] { timeout = true; loop.quit(); });
    loop.loop();
    ::shutdown(sockets[0], SHUT_WR);
    reader.join();
    conn->connectDestroyed();
    conn.reset();
    ::close(sockets[1]);
    if (replacement >= 0) ::close(replacement);
    const bool ok = !timeout && backpressured && (prematureEof
        ? closed && !complete && received == source.substr(offset)
        : complete && !closed && received == source.substr(offset, count));
    std::printf("sendfile %s: %s\n", prematureEof ? "premature EOF fails" : "offset/backpressure/closed source fd",
                ok ? "PASS" : "FAIL");
    return ok;
}

int main()
{
    const bool normal = run(false);
    const bool eof = run(true);
    return normal && eof ? 0 : 1;
}
