#ifndef PG_EATRACE_UTILITY_SPAN_H
#define PG_EATRACE_UTILITY_SPAN_H

#include "postgres.h"

#include "tcop/utility.h"

#include "pg_eatrace_span.h"

extern void buildUtilitySpan(Span* span, ProcessUtilityContext context, uint64 startNs, uint64 endNs, const char* commandTagName, const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], const uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], const uint8 utilitySpanId[PG_EATRACE_SPAN_ID_SIZE]);

#endif // PG_EATRACE_UTILITY_SPAN_H
