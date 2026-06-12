#ifndef PG_EATRACE_PLANNER_H
#define PG_EATRACE_PLANNER_H

#include "postgres.h"

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

#include "pg_eatrace_span.h"

extern void buildPlannerSpan(Span* span, Query* parse, PlannedStmt* plannedStatement, int cursorOptions, uint64 startNs, uint64 endNs, const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], const uint8 querySpanId[PG_EATRACE_SPAN_ID_SIZE]);

#endif // PG_EATRACE_PLANNER_H
