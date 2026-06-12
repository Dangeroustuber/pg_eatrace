#ifndef PG_EATRACE_QUERY_SPAN_H
#define PG_EATRACE_QUERY_SPAN_H

#include "postgres.h"

#include "executor/execdesc.h"
#include "nodes/parsenodes.h"

#include "pg_eatrace_span.h"

// Builds and enqueues the postgres.query span (plus child plan-node spans on
// success) from the trace state captured for queryDesc. No-ops when the query
// has no trace context or its span was already emitted.
extern void emitSuccessfulQuerySpan(QueryDesc* queryDesc);
extern void emitFailedQuerySpan(QueryDesc* queryDesc, const char* sqlState, const char* message);
extern void buildQuerySpanFromPlanner(Span* span, Query* parse, uint64 startNs, uint64 endNs, const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], const uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], const uint8 querySpanId[PG_EATRACE_SPAN_ID_SIZE]);

#endif // PG_EATRACE_QUERY_SPAN_H
