#include "SdkInternal.h"

extern "C" {
void cldt_client_options_init(cldt_client_options *options)
{
    if (!options) return;
    *options = cldt_client_options{};
    options->struct_size = sizeof(*options);
    options->connect_timeout_ms = 10000;
    options->io_timeout_ms = 60000;
    options->download_durability = CLDT_DURABILITY_SYNC;
}

int cldt_client_set_options(cldt_client *client, const cldt_client_options *options)
{
    if (!client || !options || options->struct_size < sizeof(*options) ||
        !options->connect_timeout_ms || !options->io_timeout_ms ||
        (options->tls_enabled != 0 && options->tls_enabled != 1) ||
        (options->download_durability != CLDT_DURABILITY_BUFFERED &&
         options->download_durability != CLDT_DURABILITY_SYNC))
        return CLDT_STATUS_INVALID_ARGUMENT;
    if (options->tls_enabled)
    {
#if !defined(CLDT_SDK_HAS_TLS)
        return CLDT_STATUS_NOT_SUPPORTED;
#endif
    }
    try
    {
        std::string ca = options->ca_file ? options->ca_file : "";
        std::string name = options->server_name ? options->server_name : "";
        std::lock_guard<std::mutex> lock(client->mutex);
        if (client->busy || client->stopping) return CLDT_STATUS_BUSY;
        client->caFile.swap(ca);
        client->serverName.swap(name);
        client->connectTimeoutMs = options->connect_timeout_ms;
        client->ioTimeoutMs = options->io_timeout_ms;
        client->tlsEnabled = options->tls_enabled != 0;
        client->durability = options->download_durability;
        return CLDT_STATUS_OK;
    }
    catch (...) { return CLDT_STATUS_INTERNAL_ERROR; }
}

int cldt_client_set_file_sync(cldt_client *client, cldt_file_sync_fn sync)
{
    if (!client) return CLDT_STATUS_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(client->mutex);
    if (client->busy || client->stopping) return CLDT_STATUS_BUSY;
    client->fileSync = sync;
    return CLDT_STATUS_OK;
}
}
