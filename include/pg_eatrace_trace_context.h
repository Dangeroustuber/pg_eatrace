#ifndef PG_EATRACE_TRACE_CONTEXT_H
#define PG_EATRACE_TRACE_CONTEXT_H

#include "postgres.h"

#include "pg_eatrace_span.h"

// Two hex chars per id byte plus the terminator.
#define PG_EATRACE_TRACE_ID_HEX_SIZE (PG_EATRACE_TRACE_ID_SIZE * 2 + 1)
#define PG_EATRACE_SPAN_ID_HEX_SIZE (PG_EATRACE_SPAN_ID_SIZE * 2 + 1)

extern uint64 secondsToNanos(double seconds);
extern uint64 getCurrentUnixTime(void);
extern void generateSpanId(uint8 spanId[PG_EATRACE_SPAN_ID_SIZE]);
extern void formatIdBytes(char* dest, const uint8* bytes, int length);
extern bool parseTraceParentValueWithSampling(const char* traceParentValue, uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], bool* sampled);
extern bool parseTraceParentFromQueryWithSampling(const char* sqlText, uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], bool* sampled);

#endif // PG_EATRACE_TRACE_CONTEXT_H
