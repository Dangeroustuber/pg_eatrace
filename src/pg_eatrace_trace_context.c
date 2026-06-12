#include "postgres.h"

#include "common/pg_prng.h"

#include "pg_eatrace_trace_context.h"

#include <time.h>

uint64 secondsToNanos(double seconds) {
    if (seconds <= 0.0) {
        return 0;
    }

    return (uint64)(seconds * 1000000000.0);
}

uint64 getCurrentUnixTime(void) {
    struct timespec timeSpec;
    clock_gettime(CLOCK_REALTIME, &timeSpec);
    return (uint64)timeSpec.tv_sec * UINT64CONST(1000000000) + (uint64)timeSpec.tv_nsec;
}

void generateSpanId(uint8 spanId[PG_EATRACE_SPAN_ID_SIZE]) {
    // Span IDs only need to be unique, not cryptographically strong, so the cheap
    // per-backend PRNG is used rather than pg_strong_random.
    uint64 randomValue = pg_prng_uint64(&pg_global_prng_state);

    for (int index = 0; index < PG_EATRACE_SPAN_ID_SIZE; index++) {
        spanId[index] = (uint8)(randomValue >> (8 * (PG_EATRACE_SPAN_ID_SIZE - 1 - index)));
    }
}

// Writes 2 * length lowercase hex chars plus a terminator into dest.
void formatIdBytes(char* dest, const uint8* bytes, int length) {
    static const char hexDigits[] = "0123456789abcdef";

    for (int index = 0; index < length; index++) {
        dest[index * 2] = hexDigits[bytes[index] >> 4];
        dest[index * 2 + 1] = hexDigits[bytes[index] & 0xf];
    }

    dest[length * 2] = '\0';
}

static int hexDigitValue(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }

    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }

    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }

    return -1;
}

static bool parseHexBytes(const char* hex, uint8* dest, int byteLength) {
    for (int index = 0; index < byteLength; index++) {
        int high = hexDigitValue(hex[index * 2]);
        int low = hexDigitValue(hex[index * 2 + 1]);

        if (high < 0 || low < 0) {
            return false;
        }

        dest[index] = (uint8)((high << 4) | low);
    }

    return true;
}

static bool isZeroBytes(const uint8* bytes, int length) {
    for (int index = 0; index < length; index++) {
        if (bytes[index] != 0) {
            return false;
        }
    }

    return true;
}

// 00-32_hex_trace_id-16_hex_span_parent_id-<2_hex_flags>
// 2 + 1 + 32 + 1 + 16 + 1 + 2 = 55
bool parseTraceParentValueWithSampling(const char* traceParentValue, uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], bool* sampled) {
    uint8 version;
    uint8 traceFlags;

    if (sampled) {
        *sampled = false;
    }

    if (strlen(traceParentValue) != 55) {
        return false;
    }

    if (traceParentValue[2] != '-' || traceParentValue[35] != '-' || traceParentValue[52] != '-') {
        return false;
    }

    if (!parseHexBytes(traceParentValue, &version, 1) || version == 0xff) {
        return false;
    }

    if (!parseHexBytes(traceParentValue + 3, traceId, PG_EATRACE_TRACE_ID_SIZE)) {
        return false;
    }

    if (!parseHexBytes(traceParentValue + 36, parentSpanId, PG_EATRACE_SPAN_ID_SIZE)) {
        return false;
    }

    if (!parseHexBytes(traceParentValue + 53, &traceFlags, 1)) {
        return false;
    }

    if (isZeroBytes(traceId, PG_EATRACE_TRACE_ID_SIZE)) {
        return false;
    }

    if (isZeroBytes(parentSpanId, PG_EATRACE_SPAN_ID_SIZE)) {
        return false;
    }

    if (sampled) {
        *sampled = (traceFlags & 0x01) != 0;
    }

    return true;
}

bool parseTraceParentFromQueryWithSampling(const char* sqlText, uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], bool* sampled) {
    const char* key = "traceparent='";
    const Size keyLen = strlen(key);
    const char* cursor = sqlText;

    if (sampled) {
        *sampled = false;
    }

    // SQLCommenter trace context lives in a /* ... */ comment. Only honor a
    // traceparent found inside a comment block, so a value appearing in a string
    // literal or elsewhere in the SQL text is not mistaken for trace context.
    while ((cursor = strstr(cursor, "/*")) != NULL) {
        const char* commentEnd = strstr(cursor + 2, "*/");
        const char* hit;

        if (!commentEnd) {
            return false;
        }

        hit = strstr(cursor + 2, key);
        if (hit && hit < commentEnd) {
            const char* valueStart = hit + keyLen;
            char value[56];

            // The 55-char value plus its closing quote must fall inside the comment.
            if (valueStart + 55 < commentEnd && valueStart[55] == '\'') {
                memcpy(value, valueStart, 55);
                value[55] = '\0';

                if (parseTraceParentValueWithSampling(value, traceId, parentSpanId, sampled)) {
                    return true;
                }
            }
        }

        cursor = commentEnd + 2;
    }

    return false;
}
