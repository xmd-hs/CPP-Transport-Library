#include "SdkInternal.h"

#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/sendfile.h>
#include <signal.h>
#include <pthread.h>
#endif

namespace cldt_sdk
{
namespace { struct NativeFile { int fd = -1; }; }

bool uploadNativeFile(cldt_client *client, NativeSocket socket, void *opaque,
                      uint64_t offset, uint64_t total, uint64_t *done, int *status)
{
#if defined(__linux__) && !defined(CLDT_SDK_DISABLE_SENDFILE)
    if (client->customFileIo)
    {
        return false;
    }
    // sendfile has no MSG_NOSIGNAL flag. Block SIGPIPE only for this upload,
    // consume a newly generated signal, then restore the caller's thread mask.
    struct SignalGuard
    {
        sigset_t blocked{}, previous{}, pending{};
        bool active = false;
        bool alreadyPending = false;
        SignalGuard()
        {
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGPIPE);
            active = pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0;
            if (active && sigpending(&pending) == 0)
            {
                alreadyPending = sigismember(&pending, SIGPIPE) == 1;
            }
        }
        ~SignalGuard()
        {
            if (active)
            {
                if (!alreadyPending)
                {
                    timespec timeout{};
                    while (sigtimedwait(&blocked, nullptr, &timeout) < 0 && errno == EINTR) {}
                }
                pthread_sigmask(SIG_SETMASK, &previous, nullptr);
            }
        }
    } guard;
    if (!guard.active)
    {
        return false;
    }
    const int fd = static_cast<NativeFile *>(opaque)->fd;
    auto lastReport = Clock::now() - std::chrono::seconds(1);
    while (*done < total)
    {
        if (cancelled(client))
        {
            *status = CLDT_STATUS_CANCELLED;
            return true;
        }
        off_t position = static_cast<off_t>(offset + *done);
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(total - *done, 1024 * 1024));
        const ssize_t sent = ::sendfile(socket, fd, &position, amount);
        if (sent > 0)
        {
            *done += static_cast<uint64_t>(sent);
            const auto now = Clock::now();
            if (now - lastReport >= std::chrono::milliseconds(100) || *done == total)
            {
                reportProgress(client, *done, total, true);
                lastReport = now;
            }
            continue;
        }
        if (sent < 0 && errno == EINTR)
        {
            continue;
        }
        if (sent < 0 && (errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP))
        {
            return false;
        }
        *status = cancelled(client) ? CLDT_STATUS_CANCELLED :
                  (sent == 0 ? CLDT_STATUS_FILE_ERROR : CLDT_STATUS_NETWORK_ERROR);
        return true;
    }
    *status = CLDT_STATUS_OK;
    return true;
#else
    (void)client; (void)socket; (void)opaque; (void)offset;
    (void)total; (void)done; (void)status;
    return false;
#endif
}
int nativeOpen(const std::string &identifier, uint32_t flags, uint64_t expectedSize,
               void **handle, uint64_t *fileSize)
{
    auto *file = new (std::nothrow) NativeFile();
    if (!file)
    {
        return -1;
    }
#if defined(_WIN32)
    const int wideLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                  identifier.c_str(), -1, nullptr, 0);
    if (wideLength <= 0)
    {
        delete file;
        return -1;
    }
    std::wstring widePath(static_cast<size_t>(wideLength), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, identifier.c_str(), -1,
                              &widePath[0], wideLength) <= 0)
    {
        delete file;
        return -1;
    }
    widePath.resize(static_cast<size_t>(wideLength - 1));
    const int access = (flags & CLDT_FILE_WRITE) ? (_O_RDWR | _O_CREAT) : _O_RDONLY;
    int fd = -1;
    if (::_wsopen_s(&fd, widePath.c_str(), access | _O_BINARY, _SH_DENYNO,
                    _S_IREAD | _S_IWRITE) != 0)
    {
        delete file;
        return -1;
    }
    if ((flags & CLDT_FILE_TRUNCATE) && ::_chsize_s(fd, expectedSize) != 0)
    {
        ::_close(fd);
        delete file;
        return -1;
    }
    const __int64 length = ::_filelengthi64(fd);
    if (length < 0)
    {
        ::_close(fd);
        delete file;
        return -1;
    }
    *fileSize = static_cast<uint64_t>(length);
    file->fd = fd;
#else
    int openFlags = (flags & CLDT_FILE_WRITE) ? (O_RDWR | O_CREAT) : O_RDONLY;
    openFlags |= O_CLOEXEC;
    const int fd = ::open(identifier.c_str(), openFlags, 0644);
    if (fd < 0)
    {
        delete file;
        return -1;
    }
    if (flags & CLDT_FILE_TRUNCATE)
    {
        if (::ftruncate(fd, static_cast<off_t>(expectedSize)) != 0)
        {
            ::close(fd);
            delete file;
            return -1;
        }
        *fileSize = expectedSize;
    }
    else
    {
        struct stat st;
        if (::fstat(fd, &st) != 0 || st.st_size < 0)
        {
            ::close(fd);
            delete file;
            return -1;
        }
        *fileSize = static_cast<uint64_t>(st.st_size);
    }
    file->fd = fd;
#endif
    *handle = file;
    return 0;
}

int openFile(cldt_client *client, const std::string &identifier, uint32_t flags,
             uint64_t expectedSize, void **handle, uint64_t *size)
{
    if (client->customFileIo)
    {
        return client->fileIo.open(client->fileIo.context, identifier.c_str(), flags,
                                   expectedSize, handle, size);
    }
    return nativeOpen(identifier, flags, expectedSize, handle, size);
}

void closeFile(cldt_client *client, void *handle)
{
    if (!handle)
    {
        return;
    }
    if (client->customFileIo)
    {
        client->fileIo.close(handle);
        return;
    }
    auto *file = static_cast<NativeFile *>(handle);
#if defined(_WIN32)
    ::_close(file->fd);
#else
    ::close(file->fd);
#endif
    delete file;
}

bool readAt(cldt_client *client, void *handle, uint64_t offset, void *buffer,
            size_t capacity, size_t *bytesRead)
{
    if (client->customFileIo)
    {
        return client->fileIo.read_at(handle, offset, buffer, capacity, bytesRead) == 0;
    }
    auto *file = static_cast<NativeFile *>(handle);
#if defined(_WIN32)
    if (::_lseeki64(file->fd, static_cast<__int64>(offset), SEEK_SET) < 0)
    {
        return false;
    }
    const unsigned amount = static_cast<unsigned>(std::min<size_t>(capacity, UINT_MAX));
    const int n = ::_read(file->fd, buffer, amount);
#else
    const ssize_t n = ::pread(file->fd, buffer, capacity, static_cast<off_t>(offset));
#endif
    if (n < 0)
    {
        return false;
    }
    *bytesRead = static_cast<size_t>(n);
    return true;
}

bool writeAt(cldt_client *client, void *handle, uint64_t offset, const void *buffer,
             size_t length, size_t *bytesWritten)
{
    if (client->customFileIo)
    {
        return client->fileIo.write_at(handle, offset, buffer, length, bytesWritten) == 0;
    }
    auto *file = static_cast<NativeFile *>(handle);
#if defined(_WIN32)
    if (::_lseeki64(file->fd, static_cast<__int64>(offset), SEEK_SET) < 0)
    {
        return false;
    }
    const unsigned amount = static_cast<unsigned>(std::min<size_t>(length, UINT_MAX));
    const int n = ::_write(file->fd, buffer, amount);
#else
    const ssize_t n = ::pwrite(file->fd, buffer, length, static_cast<off_t>(offset));
#endif
    if (n < 0)
    {
        return false;
    }
    *bytesWritten = static_cast<size_t>(n);
    return true;
}


} // namespace cldt_sdk
