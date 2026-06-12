#ifndef PG_EATRACE_PLAN_H
#define PG_EATRACE_PLAN_H

#include "postgres.h"

#include "executor/execdesc.h"

#include "pg_eatrace_span.h"

extern bool parallelWorkerSpans;

extern void definePlanGucs(void);
extern const char* planNodeTypeName(Plan* plan);
extern void enqueuePlanNodeSpans(QueryDesc* queryDesc, Span* querySpan, uint64 queryStartNs, uint64 queryEndNs);

#endif // PG_EATRACE_PLAN_H
