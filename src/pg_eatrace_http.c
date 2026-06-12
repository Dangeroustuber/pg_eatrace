#include "postgres.h"

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "utils/wait_classes.h"

#include "pg_eatrace_http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct ParsedUrl {
    char host[256];
    char port[16];
    char path[512];
} ParsedUrl;

static uint64 monotonicMs(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64)ts.tv_sec * 1000 + (uint64)ts.tv_nsec / 1000000;
}

static bool parseHttpUrl(const char* url, ParsedUrl* parsed, char* errorMessage, size_t errorMessageSize) {
    const char* cursor;
    const char* hostStart;
    size_t hostLength;

    memset(parsed, 0, sizeof(ParsedUrl));

    if (!url || pg_strncasecmp(url, "http://", 7) != 0) {
        if (url && pg_strncasecmp(url, "https://", 8) == 0) {
            snprintf(errorMessage, errorMessageSize, "https collector URLs are not supported; use plain http");
        }
        else {
            snprintf(errorMessage, errorMessageSize, "collector URL must start with http://");
        }
        return false;
    }

    cursor = url + 7;

    if (*cursor == '[') {
        // bracketed IPv6 literal, e.g. http://[::1]:4318/v1/traces
        const char* closingBracket = strchr(cursor, ']');

        if (!closingBracket) {
            snprintf(errorMessage, errorMessageSize, "unterminated IPv6 host in collector URL");
            return false;
        }

        hostStart = cursor + 1;
        hostLength = (size_t)(closingBracket - hostStart);
        cursor = closingBracket + 1;
    }
    else {
        hostStart = cursor;
        while (*cursor != '\0' && *cursor != ':' && *cursor != '/') {
            cursor++;
        }
        hostLength = (size_t)(cursor - hostStart);
    }

    if (hostLength == 0 || hostLength >= sizeof(parsed->host)) {
        snprintf(errorMessage, errorMessageSize, "missing or overlong host in collector URL");
        return false;
    }

    memcpy(parsed->host, hostStart, hostLength);

    if (*cursor == ':') {
        const char* portStart = cursor + 1;
        size_t portLength;

        cursor = portStart;
        while (*cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
        portLength = (size_t)(cursor - portStart);

        if (portLength == 0 || portLength >= sizeof(parsed->port)) {
            snprintf(errorMessage, errorMessageSize, "invalid port in collector URL");
            return false;
        }

        memcpy(parsed->port, portStart, portLength);
    }
    else {
        strlcpy(parsed->port, "80", sizeof(parsed->port));
    }

    if (*cursor == '\0') {
        strlcpy(parsed->path, "/", sizeof(parsed->path));
    }
    else if (*cursor == '/') {
        if (strlen(cursor) >= sizeof(parsed->path)) {
            snprintf(errorMessage, errorMessageSize, "path in collector URL is too long");
            return false;
        }
        strlcpy(parsed->path, cursor, sizeof(parsed->path));
    }
    else {
        snprintf(errorMessage, errorMessageSize, "unexpected character after host in collector URL");
        return false;
    }

    return true;
}

// Waits for the socket to become ready, the deadline to expire, or a shutdown
// request. Latch wakeups are consumed so the wait cannot spin; the pending
// flags they signal (shutdown, config reload) survive the reset and are
// handled here and by the worker's main loop.
static bool waitSocketReady(int sock, uint32 socketEvent, uint64 deadlineMs, const char* phase, HttpResult* result) {
    for (;;) {
        uint64 now;
        int events;

        if (ShutdownRequestPending) {
            if (result) {
                snprintf(result->errorMessage, sizeof(result->errorMessage), "shutdown requested while %s", phase);
            }
            return false;
        }

        now = monotonicMs();
        if (now >= deadlineMs) {
            if (result) {
                snprintf(result->errorMessage, sizeof(result->errorMessage), "timeout while %s", phase);
            }
            return false;
        }

        events = WaitLatchOrSocket(MyLatch,
            WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH | socketEvent,
            sock,
            (long)(deadlineMs - now),
            PG_WAIT_EXTENSION);

        if (events & WL_LATCH_SET) {
            ResetLatch(MyLatch);
            continue;
        }

        if (events & socketEvent) {
            return true;
        }
        // timed out; the loop top recomputes the remaining time and reports
    }
}

static bool setNonBlocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);

    return flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int connectToHost(const ParsedUrl* parsed, uint64 connectDeadlineMs, HttpResult* result) {
    struct addrinfo hints;
    struct addrinfo* addresses = NULL;
    struct addrinfo* address;
    int sock = -1;
    int rc;
    char lastError[PG_EATRACE_HTTP_ERROR_MESSAGE_SIZE] = "";

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(parsed->host, parsed->port, &hints, &addresses);
    if (rc != 0) {
        snprintf(result->errorMessage, sizeof(result->errorMessage), "could not resolve \"%s\": %s", parsed->host, gai_strerror(rc));
        return -1;
    }

    for (address = addresses; address != NULL; address = address->ai_next) {
        sock = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (sock < 0) {
            snprintf(lastError, sizeof(lastError), "socket failed: %s", strerror(errno));
            continue;
        }

        if (!setNonBlocking(sock)) {
            snprintf(lastError, sizeof(lastError), "could not set socket non-blocking: %s", strerror(errno));
        }
        else if (connect(sock, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        else if (errno == EINPROGRESS) {
            if (waitSocketReady(sock, WL_SOCKET_WRITEABLE, connectDeadlineMs, "connecting to collector", result)) {
                int socketError = 0;
                socklen_t socketErrorSize = sizeof(socketError);

                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorSize) == 0 && socketError == 0) {
                    break;
                }

                snprintf(lastError, sizeof(lastError), "connect to %s:%s failed: %s", parsed->host, parsed->port, strerror(socketError ? socketError : errno));
            }
            else {
                // timeout or shutdown; keep the message waitSocketReady wrote
                strlcpy(lastError, result->errorMessage, sizeof(lastError));
            }
        }
        else {
            snprintf(lastError, sizeof(lastError), "connect to %s:%s failed: %s", parsed->host, parsed->port, strerror(errno));
        }

        close(sock);
        sock = -1;
    }

    freeaddrinfo(addresses);

    if (sock < 0) {
        strlcpy(result->errorMessage, lastError[0] ? lastError : "could not connect to collector", sizeof(result->errorMessage));
        return -1;
    }

    result->errorMessage[0] = '\0';
    return sock;
}

static bool sendAll(int sock, const char* data, size_t length, uint64 deadlineMs, HttpResult* result) {
    size_t sent = 0;

    while (sent < length) {
        ssize_t written = send(sock, data + sent, length - sent, MSG_NOSIGNAL);

        if (written > 0) {
            sent += (size_t)written;
        }
        else if (errno == EINTR) {
            continue;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!waitSocketReady(sock, WL_SOCKET_WRITEABLE, deadlineMs, "sending request", result)) {
                return false;
            }
        }
        else {
            snprintf(result->errorMessage, sizeof(result->errorMessage), "send failed: %s", strerror(errno));
            return false;
        }
    }

    return true;
}

static bool parseStatusLine(const char* buffer, long* statusCode) {
    if (strncmp(buffer, "HTTP/1.", 7) != 0) {
        return false;
    }

    if (buffer[7] < '0' || buffer[7] > '9' || buffer[8] != ' ') {
        return false;
    }

    for (int index = 9; index < 12; index++) {
        if (buffer[index] < '0' || buffer[index] > '9') {
            return false;
        }
    }

    *statusCode = (buffer[9] - '0') * 100 + (buffer[10] - '0') * 10 + (buffer[11] - '0');
    return true;
}

static bool readStatusCode(int sock, uint64 deadlineMs, HttpResult* result) {
    char buffer[1024];
    size_t used = 0;

    for (;;) {
        ssize_t received = recv(sock, buffer + used, sizeof(buffer) - 1 - used, 0);

        if (received > 0) {
            used += (size_t)received;
            buffer[used] = '\0';

            if (strchr(buffer, '\n')) {
                if (parseStatusLine(buffer, &result->httpStatusCode)) {
                    return true;
                }
                snprintf(result->errorMessage, sizeof(result->errorMessage), "could not parse HTTP status line");
                return false;
            }

            if (used >= sizeof(buffer) - 1) {
                snprintf(result->errorMessage, sizeof(result->errorMessage), "HTTP status line too long");
                return false;
            }
        }
        else if (received == 0) {
            snprintf(result->errorMessage, sizeof(result->errorMessage), "connection closed before HTTP status line");
            return false;
        }
        else if (errno == EINTR) {
            continue;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!waitSocketReady(sock, WL_SOCKET_READABLE, deadlineMs, "waiting for response", result)) {
                return false;
            }
        }
        else {
            snprintf(result->errorMessage, sizeof(result->errorMessage), "recv failed: %s", strerror(errno));
            return false;
        }
    }
}

// Best-effort read until the collector closes its side (we sent Connection:
// close), so it sees a clean end of the exchange instead of a reset. The
// export verdict is already known, so failures here are ignored.
static void drainResponse(int sock, uint64 deadlineMs) {
    char discard[4096];

    for (;;) {
        ssize_t received = recv(sock, discard, sizeof(discard), 0);

        if (received > 0) {
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (waitSocketReady(sock, WL_SOCKET_READABLE, deadlineMs, "draining response", NULL)) {
                continue;
            }
        }
        return;
    }
}

bool httpPostJson(const char* url, const char* body, int connectTimeoutMs, int totalTimeoutMs, HttpResult* result) {
    ParsedUrl parsed;
    StringInfoData headers;
    uint64 startMs;
    uint64 totalDeadlineMs;
    size_t bodyLength;
    int sock;
    bool ok;

    memset(result, 0, sizeof(HttpResult));

    if (!parseHttpUrl(url, &parsed, result->errorMessage, sizeof(result->errorMessage))) {
        return false;
    }

    startMs = monotonicMs();
    totalDeadlineMs = startMs + (uint64)totalTimeoutMs;

    sock = connectToHost(&parsed, startMs + (uint64)connectTimeoutMs, result);
    if (sock < 0) {
        return false;
    }

    bodyLength = strlen(body);

    initStringInfo(&headers);
    appendStringInfo(&headers, "POST %s HTTP/1.1\r\n", parsed.path);
    appendStringInfo(&headers, strchr(parsed.host, ':') ? "Host: [%s]" : "Host: %s", parsed.host);
    if (strcmp(parsed.port, "80") != 0) {
        appendStringInfo(&headers, ":%s", parsed.port);
    }
    appendStringInfoString(&headers, "\r\n");
    appendStringInfoString(&headers, "Content-Type: application/json\r\n");
    appendStringInfo(&headers, "Content-Length: %zu\r\n", bodyLength);
    appendStringInfoString(&headers, "Connection: close\r\n\r\n");

    // The body is sent separately to avoid copying a potentially large batch
    // document into the header buffer.
    ok = sendAll(sock, headers.data, (size_t)headers.len, totalDeadlineMs, result)
        && sendAll(sock, body, bodyLength, totalDeadlineMs, result)
        && readStatusCode(sock, totalDeadlineMs, result);

    if (ok) {
        drainResponse(sock, totalDeadlineMs);
    }

    close(sock);
    pfree(headers.data);

    return ok;
}

bool httpUrlIsValid(const char* url, char* errorMessage, size_t errorMessageSize) {
    ParsedUrl parsed;

    return parseHttpUrl(url, &parsed, errorMessage, errorMessageSize);
}
