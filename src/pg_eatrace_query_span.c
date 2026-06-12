#include "postgres.h"

#include "executor/instrument.h"
#include "tcop/cmdtag.h"
#include "tcop/utility.h"

#include "pg_eatrace_query_span.h"
#include "pg_eatrace_plan.h"
#include "pg_eatrace_queue.h"
#include "pg_eatrace_span.h"
#include "pg_eatrace_trace_state.h"
#include "pg_eatrace_trace_context.h"

static double finishTotalTime(QueryDesc* queryDesc) {
    if (queryDesc->totaltime == NULL) {
        return 0.0;
    }

    // it is okay if another executor hook also calls this
    InstrEndLoop(queryDesc->totaltime);
    return queryDesc->totaltime->total;
}

static void fillQueryResourceData(QuerySpanData* data, QueryDesc* queryDesc) {
    if (queryDesc->totaltime) {
        data->hasResourceUsage = true;
        data->bufferUsage = queryDesc->totaltime->bufusage;
        data->walUsage = queryDesc->totaltime->walusage;
    }

    if (queryDesc->estate) {
        data->hasJitFlags = true;
        data->jitFlags = queryDesc->estate->es_jit_flags;

        if (queryDesc->estate->es_jit) {
            data->hasJit = true;
            data->jit = queryDesc->estate->es_jit->instr;
        }

        if (queryDesc->estate->es_jit_worker_instr) {
            data->hasWorkerJit = true;
            data->workerJit = *queryDesc->estate->es_jit_worker_instr;
        }
    }
}

static void buildQuerySpan(Span* span, QueryTraceState* queryState, QueryDesc* queryDesc, uint64 startNs, uint64 endNs) {
    initSpan(span, PG_EATRACE_SPAN_QUERY, queryState->traceId, queryState->parentSpanId, queryState->querySpanId, "postgres.query", startNs, endNs);

    fillQueryResourceData(&span->data.query, queryDesc);
}

// Builds the errored root query span for a statement whose planning failed, so
// execution-side resource usage never exists; the command tag stands in.
void buildQuerySpanFromPlanner(Span* span, Query* parse, uint64 startNs, uint64 endNs, const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], const uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], const uint8 querySpanId[PG_EATRACE_SPAN_ID_SIZE]) {
    const char* commandTagName = "UNKNOWN";

    if (parse) {
        commandTagName = GetCommandTagName(CreateCommandTag((Node*)parse));
    }

    initSpan(span, PG_EATRACE_SPAN_QUERY, traceId, parentSpanId, querySpanId, "postgres.query", startNs, endNs);

    strlcpy(span->data.query.commandTag, commandTagName, sizeof(span->data.query.commandTag));
}

void emitFailedQuerySpan(QueryDesc* queryDesc, const char* sqlState, const char* message) {
    QueryTraceState* queryState = findQueryState(queryDesc);

    if (queryState && queryState->hasTraceParent && !queryState->spanEmitted) {
        Span span;
        uint64 endNs = getCurrentUnixTime();

        buildQuerySpan(&span, queryState, queryDesc, queryState->startTimeUnixNano, endNs);
        addErrorToSpan(&span, sqlState, message);
        enqueueSpan(&span);
        queryState->spanEmitted = true;
    }
}

void emitSuccessfulQuerySpan(QueryDesc* queryDesc) {
    QueryTraceState* queryState = findQueryState(queryDesc);

    if (queryState && queryState->hasTraceParent && !queryState->spanEmitted) {
        Span span;
        double totalTimeSeconds = finishTotalTime(queryDesc);
        uint64 totalTimeNs = secondsToNanos(totalTimeSeconds);
        uint64 endNs = getCurrentUnixTime();
        uint64 startNs;

        if (queryState->hasPlannerState) {
            startNs = queryState->startTimeUnixNano;
        }
        else if (totalTimeNs > 0 && endNs >= totalTimeNs) {
            startNs = endNs - totalTimeNs;
        }
        else {
            startNs = queryState->startTimeUnixNano;
        }

        buildQuerySpan(&span, queryState, queryDesc, startNs, endNs);
        enqueueSpan(&span);
        enqueuePlanNodeSpans(queryDesc, &span, startNs, endNs);
        queryState->spanEmitted = true;
    }
}
