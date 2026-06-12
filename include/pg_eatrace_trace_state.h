#ifndef PG_EATRACE_TRACE_STATE_H
#define PG_EATRACE_TRACE_STATE_H

#include "postgres.h"

#include "executor/execdesc.h"
#include "nodes/plannodes.h"

#include "pg_eatrace_span.h"

// Per-query trace context captured in ExecutorStart and consumed in ExecutorEnd.
// Keyed by QueryDesc* so nested executor invocations do not overwrite each other.
typedef struct QueryTraceState {
    QueryDesc* queryDesc;
    bool hasTraceParent;
    bool hasPlannerState;
    bool spanEmitted;
    // Index of this query's frame on the active-span stack, or -1 when none.
    int activeSpanIndex;
    uint8 traceId[PG_EATRACE_TRACE_ID_SIZE];
    uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE];
    uint8 querySpanId[PG_EATRACE_SPAN_ID_SIZE];
    uint64 startTimeUnixNano;
} QueryTraceState;

typedef struct PlannerTraceState {
    PlannedStmt* plannedStatement;
    bool hasTraceParent;
    uint8 traceId[PG_EATRACE_TRACE_ID_SIZE];
    uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE];
    uint8 querySpanId[PG_EATRACE_SPAN_ID_SIZE];
    uint64 startTimeUnixNano;
} PlannerTraceState;

extern QueryTraceState* createQueryState(QueryDesc* queryDesc);
extern QueryTraceState* findQueryState(QueryDesc* queryDesc);
extern void deleteQueryState(QueryDesc* queryDesc);
extern PlannerTraceState* createPlannerState(PlannedStmt* plannedStatement);
extern PlannerTraceState* findPlannerState(PlannedStmt* plannedStatement);
extern void deletePlannerState(PlannedStmt* plannedStatement);
extern void deleteAllTraceStates(void);

#endif // PG_EATRACE_TRACE_STATE_H
