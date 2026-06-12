#include "postgres.h"

#include "pg_eatrace_span.h"
#include "pg_eatrace_trace_context.h"

void initSpan(Span* span, SpanType type, const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], const uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], const uint8 spanId[PG_EATRACE_SPAN_ID_SIZE], const char* name, uint64 startNs, uint64 endNs) {
    memset(span, 0, sizeof(Span));

    if (traceId) {
        memcpy(span->traceId, traceId, PG_EATRACE_TRACE_ID_SIZE);
    }

    if (parentSpanId) {
        memcpy(span->parentSpanId, parentSpanId, PG_EATRACE_SPAN_ID_SIZE);
    }

    if (spanId) {
        memcpy(span->spanId, spanId, PG_EATRACE_SPAN_ID_SIZE);
    }
    else {
        generateSpanId(span->spanId);
    }

    span->type = type;
    strlcpy(span->name, name ? name : "", sizeof(span->name));
    span->startTimeUnixNano = startNs;
    span->endTimeUnixNano = endNs;
}

void addErrorToSpan(Span* span, const char* sqlState, const char* message) {
    if (!span) {
        return;
    }

    span->statusCode = PG_EATRACE_SPAN_STATUS_ERROR;
    strlcpy(span->statusMessage, message ? message : "", sizeof(span->statusMessage));
    strlcpy(span->sqlState, sqlState ? sqlState : "", sizeof(span->sqlState));
}
