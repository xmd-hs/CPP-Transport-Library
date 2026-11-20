#pragma once
#if !defined(_WIN32)
#include <signal.h>
#include <pthread.h>

namespace cldt_sdk
{
// OpenSSL's socket BIO and sendfile can raise SIGPIPE. Never change the host
// application's process-wide handler; only guard the SDK worker's lifetime.
class WorkerSignalGuard
{
    sigset_t blocked_{}, previous_{};
    bool active_ = false;
public:
    WorkerSignalGuard()
    {
        sigemptyset(&blocked_);
        sigaddset(&blocked_, SIGPIPE);
        active_ = pthread_sigmask(SIG_BLOCK, &blocked_, &previous_) == 0;
    }
    ~WorkerSignalGuard()
    {
        if (!active_) return;
        sigset_t pending{};
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
        {
            int signal = 0;
            sigwait(&blocked_, &signal);
        }
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }
};
}
#else
namespace cldt_sdk { struct WorkerSignalGuard {}; }
#endif
