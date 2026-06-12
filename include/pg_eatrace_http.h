#ifndef PG_EATRACE_HTTP_H
#define PG_EATRACE_HTTP_H

#include "postgres.h"

#define PG_EATRACE_HTTP_ERROR_MESSAGE_SIZE 256

// Outcome of one export POST. httpStatusCode is 0 when no complete HTTP
// response was received; errorMessage is empty on success.
typedef struct HttpResult {
    long httpStatusCode;
    char errorMessage[PG_EATRACE_HTTP_ERROR_MESSAGE_SIZE];
} HttpResult;

// Minimal OTLP export client: plain http:// only, HTTP/1.1, Connection:
// close, no redirects/proxies/TLS. All socket waits go through
// WaitLatchOrSocket, so the worker stays responsive to shutdown requests and
// postmaster death while an export is in flight.
//
// Returns true when a complete HTTP response was received, whatever its
// status code; the caller decides what to do with non-2xx statuses.
extern bool httpPostJson(const char* url, const char* body, int connectTimeoutMs, int totalTimeoutMs, HttpResult* result);

// Validates a collector URL without connecting; used by the GUC check hook.
extern bool httpUrlIsValid(const char* url, char* errorMessage, size_t errorMessageSize);

#endif // PG_EATRACE_HTTP_H
