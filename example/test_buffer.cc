#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include "Buffer.h"

int main()
{
    int failed = 0;
    auto check = [&](bool ok, const char *name) {
        std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
        if (!ok) ++failed;
    };
    Buffer buffer;
    const std::string payload(65535, 'p');
    for (int i = 0; i < 8; ++i) buffer.append(payload.data(), payload.size());
    check(buffer.storageBytes() == 512 * 1024, "512KiB minus prepend stays in 512KiB pool class");
    buffer.retrieve(250000);
    std::string expected(buffer.peek(), buffer.readableBytes());
    const std::string tail(600000, 'q');
    buffer.append(tail.data(), tail.size());
    expected += tail;
    check(buffer.retrieveAllAsString() == expected, "growth preserves only live bytes");
    buffer.append("abcdef", 6);
    buffer.retrieve(3);
    const char prefix[] = "HEAD";
    buffer.prepend(prefix, 4);
    check(buffer.retrieveAllAsString() == "HEADdef", "prepend after partial retrieve");
    Buffer compact(32);
    compact.append("1234567890", 10);
    compact.retrieve(8);
    const std::string extra(40, 'z');
    compact.append(extra.data(), extra.size());
    check(compact.retrieveAllAsString() == "90" + extra, "compaction preserves content");
    bool overflow = false;
    try { buffer.reserve(std::numeric_limits<size_t>::max()); }
    catch (const std::length_error &) { overflow = true; }
    check(overflow, "reject capacity overflow");
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) != 0) return 1;
    std::string sent(100000, 's');
    const auto n = ::send(sockets[0], sent.data(), sent.size(), 0);
    Buffer received;
    int error = 0;
    const auto count = received.readFd(sockets[1], &error);
    check(n > 0 && count == n && received.retrieveAllAsString() == sent.substr(0, size_t(n)),
          "readv overflow buffer preserves bytes");
    ::close(sockets[0]);
    ::close(sockets[1]);
    return failed ? 1 : 0;
}
