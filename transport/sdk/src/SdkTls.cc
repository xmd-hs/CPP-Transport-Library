#include "SdkInternal.h"
#if defined(CLDT_SDK_HAS_TLS)
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#endif
#endif

namespace cldt_sdk
{
#if defined(CLDT_SDK_HAS_TLS)
bool startTls(Stream &stream, Clock::time_point deadline)
{
    auto *context = SSL_CTX_new(TLS_client_method());
    stream.tlsContext = context;
    if (!context || SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1)
    {
        stream.error = CLDT_STATUS_TLS_ERROR;
        return false;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
    const auto &ca = stream.client->caFile;
    const bool trusted = ca.empty() ? SSL_CTX_set_default_verify_paths(context) == 1
                                    : SSL_CTX_load_verify_locations(context, ca.c_str(), nullptr) == 1;
    if (!trusted) { stream.error = CLDT_STATUS_TLS_ERROR; return false; }
    auto *ssl = SSL_new(context);
    stream.tls = ssl;
    if (!ssl) { stream.error = CLDT_STATUS_TLS_ERROR; return false; }
    const std::string &name = stream.client->serverName.empty()
        ? stream.client->host : stream.client->serverName;
    unsigned char binary[16];
    const bool ip = inet_pton(AF_INET, name.c_str(), binary) == 1 ||
                    inet_pton(AF_INET6, name.c_str(), binary) == 1;
    auto *parameters = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    const bool identity = ip ? X509_VERIFY_PARAM_set1_ip_asc(parameters, name.c_str()) == 1
                             : SSL_set1_host(ssl, name.c_str()) == 1 &&
                               SSL_set_tlsext_host_name(ssl, name.c_str()) == 1;
    if (!identity || SSL_set_fd(ssl, static_cast<int>(stream.fd)) != 1)
    {
        stream.error = CLDT_STATUS_TLS_ERROR;
        return false;
    }
    for (;;)
    {
        if (cancelled(stream.client)) { stream.error = CLDT_STATUS_CANCELLED; return false; }
        if (Clock::now() >= deadline) { stream.error = CLDT_STATUS_TIMEOUT; return false; }
        ERR_clear_error();
        const int result = SSL_connect(ssl);
        if (result == 1)
        {
            if (SSL_get_verify_result(ssl) != X509_V_OK)
            {
                stream.error = CLDT_STATUS_TLS_ERROR;
                return false;
            }
            return true;
        }
        const int error = SSL_get_error(ssl, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
        {
            stream.error = CLDT_STATUS_TLS_ERROR;
            return false;
        }
        if (!waitSocket(stream, error == SSL_ERROR_WANT_WRITE, deadline)) return false;
    }
}
int tlsResult(Stream &stream, int result)
{
    if (result > 0) return result;
    const int error = SSL_get_error(static_cast<SSL *>(stream.tls), result);
    if (error == SSL_ERROR_WANT_READ) return -2;
    if (error == SSL_ERROR_WANT_WRITE) return -3;
    stream.error = CLDT_STATUS_NETWORK_ERROR;
    return -1;
}
int tlsRead(Stream &stream, void *data, int size)
{
    ERR_clear_error();
    return tlsResult(stream, SSL_read(static_cast<SSL *>(stream.tls), data, size));
}
int tlsWrite(Stream &stream, const void *data, int size)
{
    ERR_clear_error();
    return tlsResult(stream, SSL_write(static_cast<SSL *>(stream.tls), data, size));
}
void freeTls(Stream &stream)
{
    // Every CLDT request uses its own connection. Do not block destroy waiting
    // for a peer close_notify; the framed length defines transfer completion.
    if (stream.tls) SSL_free(static_cast<SSL *>(stream.tls));
    if (stream.tlsContext) SSL_CTX_free(static_cast<SSL_CTX *>(stream.tlsContext));
    stream.tls = nullptr;
    stream.tlsContext = nullptr;
}
#else
bool startTls(Stream &stream, Clock::time_point)
{
    stream.error = CLDT_STATUS_NOT_SUPPORTED;
    return false;
}
void freeTls(Stream &) {}
int tlsRead(Stream &, void *, int) { return -1; }
int tlsWrite(Stream &, const void *, int) { return -1; }
#endif
} // namespace cldt_sdk
