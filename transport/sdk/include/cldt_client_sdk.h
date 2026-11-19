#ifndef CLDT_CLIENT_SDK_H
#define CLDT_CLIENT_SDK_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(CLDT_SDK_SHARED)
#if defined(CLDT_SDK_BUILD)
#define CLDT_SDK_API __declspec(dllexport)
#else
#define CLDT_SDK_API __declspec(dllimport)
#endif
#else
#define CLDT_SDK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cldt_client cldt_client;

enum cldt_file_flags
{
    CLDT_FILE_READ = 1,
    CLDT_FILE_WRITE = 2,
    CLDT_FILE_TRUNCATE = 4
};

enum cldt_status
{
    CLDT_STATUS_OK = 0,
    CLDT_STATUS_INVALID_ARGUMENT = 1001,
    CLDT_STATUS_BUSY = 1002,
    CLDT_STATUS_CANCELLED = 1003,
    CLDT_STATUS_CONNECT_FAILED = 1004,
    CLDT_STATUS_NETWORK_ERROR = 1005,
    CLDT_STATUS_PROTOCOL_ERROR = 1006,
    CLDT_STATUS_FILE_ERROR = 1007,
    CLDT_STATUS_REMOTE_ERROR = 1008,
    CLDT_STATUS_INTERNAL_ERROR = 1009,
    CLDT_STATUS_REMOTE_ERROR_BASE = 2000
};

typedef int (*cldt_file_open_fn)(void *context,
                                 const char *identifier,
                                 uint32_t flags,
                                 uint64_t expected_size,
                                 void **handle,
                                 uint64_t *file_size);
typedef int (*cldt_file_read_fn)(void *handle,
                                 uint64_t offset,
                                 void *buffer,
                                 size_t capacity,
                                 size_t *bytes_read);
typedef int (*cldt_file_write_fn)(void *handle,
                                  uint64_t offset,
                                  const void *buffer,
                                  size_t length,
                                  size_t *bytes_written);
typedef void (*cldt_file_close_fn)(void *handle);

typedef struct cldt_file_io
{
    size_t struct_size;
    void *context;
    cldt_file_open_fn open;
    cldt_file_read_fn read_at;
    cldt_file_write_fn write_at;
    cldt_file_close_fn close;
} cldt_file_io;

typedef void (*cldt_progress_fn)(void *user_data,
                                 uint64_t transferred,
                                 uint64_t total,
                                 int is_upload);
typedef void (*cldt_done_fn)(void *user_data,
                             int status,
                             uint64_t total,
                             const char *message);

// Creates a serial asynchronous client. Callbacks run on its worker thread;
// applications should dispatch UI updates to their own main/UI thread.
CLDT_SDK_API cldt_client *cldt_client_create(const char *host,
                                             uint16_t port,
                                             const char *token,
                                             cldt_progress_fn progress,
                                             cldt_done_fn done,
                                             void *user_data);

// Optional. Configure before submitting work. When omitted, local identifiers
// are treated as UTF-8 native file paths. Custom I/O is intended for Android
// content URIs and iOS document/security-scoped handles. The SDK calls these
// callbacks on its worker thread; close() must flush writes before returning.
CLDT_SDK_API int cldt_client_set_file_io(cldt_client *client,
                                         const cldt_file_io *file_io);

// Requests are asynchronous. One request may be active per client instance;
// another submission returns CLDT_STATUS_BUSY until its completion callback.
CLDT_SDK_API int cldt_client_upload(cldt_client *client,
                                    const char *local_identifier,
                                    const char *remote_relative_path,
                                    uint64_t offset);
CLDT_SDK_API int cldt_client_download(cldt_client *client,
                                      const char *remote_relative_path,
                                      const char *local_identifier,
                                      uint64_t offset,
                                      uint64_t length);
CLDT_SDK_API int cldt_client_stat(cldt_client *client,
                                  const char *remote_relative_path);

// Cancels an active request by interrupting its socket. Completion reports
// CLDT_STATUS_CANCELLED or the underlying I/O error.
CLDT_SDK_API int cldt_client_cancel(cldt_client *client);

// Stops the worker and waits for callbacks/operations to finish. Do not call
// this from one of this client's callbacks.
CLDT_SDK_API void cldt_client_destroy(cldt_client *client);

#ifdef __cplusplus
}
#endif

#endif
