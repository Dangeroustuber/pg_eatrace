#include "postgres.h"

#include "access/parallel.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "storage/ipc.h"
#include "tcop/cmdtag.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "pg_eatrace_plan.h"
#include "pg_eatrace_planner.h"
#include "pg_eatrace_query_span.h"
#include "pg_eatrace_queue.h"
#include "pg_eatrace_span.h"
#include "pg_eatrace_error.h"
#include "pg_eatrace_trace_context.h"
#include "pg_eatrace_trace_state.h"
#include "pg_eatrace_utility_span.h"
#include "pg_eatrace_worker.h"

// must have this in the file so postgresql loads it
PG_MODULE_MAGIC;

// we need to be able to save the previous hook.
static ExecutorEnd_hook_type previousExecutorEnd = NULL;
static ExecutorFinish_hook_type previousExecutorFinish = NULL;
static ExecutorRun_hook_type previousExecutorRun = NULL;
static ExecutorStart_hook_type previousExecutorStart = NULL;
static planner_hook_type previousPlanner = NULL;
static ProcessUtility_hook_type previousProcessUtility = NULL;
static shmem_request_hook_type previousShmemRequestHook = NULL;
static shmem_startup_hook_type previousShmemStartupHook = NULL;

static bool traceEnabled = true;
static double sampleRate = 1.0;

typedef enum SpanGenerationMode {
    PG_EATRACE_SPAN_GENERATION_NONE,
    PG_EATRACE_SPAN_GENERATION_TOP_LEVEL,
    PG_EATRACE_SPAN_GENERATION_ALL
} SpanGenerationMode;

static const struct config_enum_entry spanGenerationOptions[] = {
    { "none", PG_EATRACE_SPAN_GENERATION_NONE, false },
    { "top-level", PG_EATRACE_SPAN_GENERATION_TOP_LEVEL, false },
    { "all", PG_EATRACE_SPAN_GENERATION_ALL, false },
    { NULL, 0, false }
};

static int spanGeneration = PG_EATRACE_SPAN_GENERATION_TOP_LEVEL;

typedef struct ActiveSpanFrame {
    uint8 traceId[PG_EATRACE_TRACE_ID_SIZE];
    uint8 spanId[PG_EATRACE_SPAN_ID_SIZE];
} ActiveSpanFrame;

#define PG_EATRACE_MAX_ACTIVE_SPANS 64

static ActiveSpanFrame activeSpanStack[PG_EATRACE_MAX_ACTIVE_SPANS];
static int activeSpanDepth = 0;

static void defineTracingGucs(void) {
    DefineCustomBoolVariable("pg_eatrace.enabled",
        "Enable pg_eatrace span collection in backend hooks.",
        NULL,
        &traceEnabled,
        true,
        PGC_SUSET,
        0,
        NULL,
        NULL,
        NULL);
    DefineCustomEnumVariable("pg_eatrace.span_generation",
        "Controls which statement levels emit pg_eatrace spans.",
        "top-level traces only top-level statements, all also traces nested statements, and none disables span generation while leaving the extension enabled.",
        &spanGeneration,
        PG_EATRACE_SPAN_GENERATION_TOP_LEVEL,
        spanGenerationOptions,
        PGC_SUSET,
        0,
        NULL,
        NULL,
        NULL);
    DefineCustomRealVariable("pg_eatrace.sample_rate",
        "Fraction of caller-sampled trace contexts to keep.",
        "A value of 1.0 keeps every traceparent with the W3C sampled flag set. A value of 0.0 keeps none.",
        &sampleRate,
        1.0,
        0.0,
        1.0,
        PGC_SUSET,
        0,
        NULL,
        NULL,
        NULL);
}

static bool spanGenerationEnabled(void) {
    // Parallel workers re-run the executor hooks on the leader's query text,
    // including its traceparent comment. The leader already emits the query and
    // plan-node spans, so workers must not produce duplicates.
    if (IsParallelWorker()) {
        return false;
    }

    return traceEnabled && spanGeneration != PG_EATRACE_SPAN_GENERATION_NONE;
}

static bool hasActiveSpan(void) {
    return activeSpanDepth > 0;
}

static ActiveSpanFrame* currentActiveSpan(void) {
    if (!hasActiveSpan()) {
        return NULL;
    }

    return &activeSpanStack[activeSpanDepth - 1];
}

// Returns the pushed frame's index, or -1 when the stack is full.
static int pushActiveSpan(const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], const uint8 spanId[PG_EATRACE_SPAN_ID_SIZE]) {
    ActiveSpanFrame* frame;

    if (activeSpanDepth >= PG_EATRACE_MAX_ACTIVE_SPANS) {
        return -1;
    }

    frame = &activeSpanStack[activeSpanDepth];
    memcpy(frame->traceId, traceId, PG_EATRACE_TRACE_ID_SIZE);
    memcpy(frame->spanId, spanId, PG_EATRACE_SPAN_ID_SIZE);

    return activeSpanDepth++;
}

// Pops the frame at index along with any frames leaked above it by nested
// statements that never reached their own cleanup. A -1 index is a no-op.
static void popActiveSpanToIndex(int index) {
    if (index >= 0 && activeSpanDepth > index) {
        activeSpanDepth = index;
    }
}

// Deterministic per-trace decision (the OTel TraceIdRatioBased sampler): the
// random low 8 bytes of the trace id decide, so every statement in a trace —
// and any other participant using the same convention — agrees.
static bool passesLocalSampleRate(const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE]) {
    uint64 traceValue = 0;

    if (sampleRate <= 0.0) {
        return false;
    }

    if (sampleRate >= 1.0) {
        return true;
    }

    for (int index = 8; index < PG_EATRACE_TRACE_ID_SIZE; index++) {
        traceValue = (traceValue << 8) | traceId[index];
    }

    return traceValue < (uint64)(sampleRate * (double)PG_UINT64_MAX);
}

static bool findTraceContextForStatement(const char* queryString, uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE]) {
    bool sampled = false;
    ActiveSpanFrame* activeSpan;

    if (spanGeneration == PG_EATRACE_SPAN_GENERATION_NONE) {
        return false;
    }

    if (hasActiveSpan() && spanGeneration == PG_EATRACE_SPAN_GENERATION_TOP_LEVEL) {
        return false;
    }

    if (queryString && parseTraceParentFromQueryWithSampling(queryString, traceId, parentSpanId, &sampled)) {
        if (!sampled) {
            return false;
        }

        return passesLocalSampleRate(traceId);
    }

    if (spanGeneration != PG_EATRACE_SPAN_GENERATION_ALL) {
        return false;
    }

    activeSpan = currentActiveSpan();
    if (!activeSpan) {
        return false;
    }

    memcpy(traceId, activeSpan->traceId, PG_EATRACE_TRACE_ID_SIZE);
    memcpy(parentSpanId, activeSpan->spanId, PG_EATRACE_SPAN_ID_SIZE);

    return true;
}

static void shmemRequestHook(void) {
    if (previousShmemRequestHook) {
        previousShmemRequestHook();
    }

    requestQueueShmem();
}

static void shmemStartupHook(void) {
    if (previousShmemStartupHook) {
        previousShmemStartupHook();
    }

    initQueueShmem();
}

static void deleteTraceStates(QueryDesc* queryDesc) {
    QueryTraceState* queryState = findQueryState(queryDesc);

    if (queryState) {
        popActiveSpanToIndex(queryState->activeSpanIndex);
    }

    deleteQueryState(queryDesc);
    if (queryDesc && queryDesc->plannedstmt) {
        deletePlannerState(queryDesc->plannedstmt);
    }
}

// Trace states are normally torn down by the executor hooks, but paths that
// bypass ExecutorEnd (portals dropped during abort, plans built but never
// executed) leave entries keyed by soon-recycled pointers and leave active-span
// frames pushed. Reset everything once the transaction is over.
static void xactEndCallback(XactEvent event, void* arg) {
    switch (event) {
    case XACT_EVENT_ABORT:
    case XACT_EVENT_PARALLEL_ABORT:
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_PARALLEL_COMMIT:
    case XACT_EVENT_PREPARE:
        deleteAllTraceStates();
        activeSpanDepth = 0;
        break;
    default:
        break;
    }
}

// Shared body for the executor hook PG_CATCH paths: emit an errored query span
// and tear down trace state. The caller must still PG_RE_THROW.
static void handleExecutorError(QueryDesc* queryDesc) {
    CaughtError error;

    copyCaughtError(&error);
    emitFailedQuerySpan(queryDesc, error.sqlState, error.message);
    deleteTraceStates(queryDesc);
}

static PlannedStmt* plannerHook(Query* parse, const char* queryString, int cursorOptions, ParamListInfo boundParams) {
    PlannedStmt* plannedStatement = NULL;
    uint8 traceId[PG_EATRACE_TRACE_ID_SIZE];
    uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE];
    uint8 querySpanId[PG_EATRACE_SPAN_ID_SIZE];
    bool hasTraceParent = false;
    uint64 startNs;

    if (!spanGenerationEnabled()) {
        if (previousPlanner) {
            return previousPlanner(parse, queryString, cursorOptions, boundParams);
        }

        return standard_planner(parse, queryString, cursorOptions, boundParams);
    }

    memset(traceId, 0, sizeof(traceId));
    memset(parentSpanId, 0, sizeof(parentSpanId));
    memset(querySpanId, 0, sizeof(querySpanId));

    hasTraceParent = findTraceContextForStatement(queryString, traceId, parentSpanId);

    if (hasTraceParent) {
        generateSpanId(querySpanId);
    }

    startNs = getCurrentUnixTime();

    PG_TRY();
    {
        if (previousPlanner) {
            plannedStatement = previousPlanner(parse, queryString, cursorOptions, boundParams);
        }
        else {
            plannedStatement = standard_planner(parse, queryString, cursorOptions, boundParams);
        }
    }
    PG_CATCH();
    {
        CaughtError error;

        copyCaughtError(&error);

        if (hasTraceParent) {
            Span querySpan;
            Span span;
            uint64 endNs = getCurrentUnixTime();

            buildQuerySpanFromPlanner(&querySpan, parse, startNs, endNs, traceId, parentSpanId, querySpanId);
            addErrorToSpan(&querySpan, error.sqlState, error.message);
            enqueueSpan(&querySpan);

            buildPlannerSpan(&span, parse, NULL, cursorOptions, startNs, endNs, traceId, querySpanId);
            addErrorToSpan(&span, error.sqlState, error.message);
            enqueueSpan(&span);
        }

        PG_RE_THROW();
    }
    PG_END_TRY();

    if (hasTraceParent) {
        PlannerTraceState* plannerState = createPlannerState(plannedStatement);
        uint64 endNs = getCurrentUnixTime();

        plannerState->hasTraceParent = true;
        memcpy(plannerState->traceId, traceId, sizeof(plannerState->traceId));
        memcpy(plannerState->parentSpanId, parentSpanId, sizeof(plannerState->parentSpanId));
        memcpy(plannerState->querySpanId, querySpanId, sizeof(plannerState->querySpanId));
        plannerState->startTimeUnixNano = startNs;

        // Defer the enqueue until the query span is emitted (emitSuccessfulQuerySpan /
        // emitFailedQuerySpan). A cached/prepared/nested statement can plan here yet
        // emit its query span under a different id, or never reach ExecutorEnd at all;
        // enqueuing now would leave a planning span whose query-span parent is never
        // exported. Tying the two together keeps the planner span from orphaning.
        buildPlannerSpan(&plannerState->plannerSpan, parse, plannedStatement, cursorOptions, startNs, endNs, traceId, querySpanId);
        plannerState->hasPlannerSpan = true;
    }

    return plannedStatement;
}

static void executorStartHook(QueryDesc* queryDesc, int eFlags) {
    bool traceThisQuery = false;

    if (spanGenerationEnabled()) {
        QueryTraceState* queryState = createQueryState(queryDesc);
        PlannerTraceState* plannerState = findPlannerState(queryDesc->plannedstmt);

        if (plannerState && plannerState->hasTraceParent) {
            queryState->hasTraceParent = true;
            queryState->hasPlannerState = true;
            memcpy(queryState->traceId, plannerState->traceId, sizeof(queryState->traceId));
            memcpy(queryState->parentSpanId, plannerState->parentSpanId, sizeof(queryState->parentSpanId));
            memcpy(queryState->querySpanId, plannerState->querySpanId, sizeof(queryState->querySpanId));
            queryState->startTimeUnixNano = plannerState->startTimeUnixNano;
        }
        else {
            queryState->startTimeUnixNano = getCurrentUnixTime();

            if (findTraceContextForStatement(queryDesc->sourceText, queryState->traceId, queryState->parentSpanId)) {
                queryState->hasTraceParent = true;
                generateSpanId(queryState->querySpanId);
            }
        }

        if (queryState->hasTraceParent) {
            traceThisQuery = true;
            queryState->activeSpanIndex = pushActiveSpan(queryState->traceId, queryState->querySpanId);

            // enable per plan node instrumentation, only for traced statements:
            // INSTRUMENT_TIMER on every node is far too expensive to pay for
            // statements that will never emit a span.
            queryDesc->instrument_options |= INSTRUMENT_TIMER | INSTRUMENT_BUFFERS | INSTRUMENT_WAL;
        }
    }

    if (!traceThisQuery) {
        // Also clears any stale state left at a recycled QueryDesc/PlannedStmt
        // address, so a later lookup cannot adopt another query's trace context.
        deleteQueryState(queryDesc);
        if (queryDesc->plannedstmt) {
            deletePlannerState(queryDesc->plannedstmt);
        }
    }

    PG_TRY();
    {
        // if there was a hook before, then we call that, otherwise just call standard postgresql one.
        if (previousExecutorStart) {
            previousExecutorStart(queryDesc, eFlags);
        }
        else {
            standard_ExecutorStart(queryDesc, eFlags);
        }

        if (traceThisQuery && queryDesc->totaltime == NULL) {
            MemoryContext oldContext;
            oldContext = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
            queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_TIMER | INSTRUMENT_BUFFERS | INSTRUMENT_WAL, false);
            MemoryContextSwitchTo(oldContext);
        }
        else if (traceThisQuery) {
            queryDesc->totaltime->need_timer = true;
            queryDesc->totaltime->need_bufusage = true;
            queryDesc->totaltime->need_walusage = true;
        }
    }
    PG_CATCH();
    {
        handleExecutorError(queryDesc);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

static void executorRunHook(QueryDesc* queryDesc, ScanDirection direction, uint64 count) {
    PG_TRY();
    {
        if (previousExecutorRun) {
            previousExecutorRun(queryDesc, direction, count);
        }
        else {
            standard_ExecutorRun(queryDesc, direction, count);
        }
    }
    PG_CATCH();
    {
        handleExecutorError(queryDesc);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

static void executorFinishHook(QueryDesc* queryDesc) {
    PG_TRY();
    {
        if (previousExecutorFinish) {
            previousExecutorFinish(queryDesc);
        }
        else {
            standard_ExecutorFinish(queryDesc);
        }
    }
    PG_CATCH();
    {
        handleExecutorError(queryDesc);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

static void executorEndHook(QueryDesc* queryDesc) {
    emitSuccessfulQuerySpan(queryDesc);
    deleteTraceStates(queryDesc);

    // if there was a hook before, then call that, otherwise just call standard postgresql one.
    if (previousExecutorEnd) {
        previousExecutorEnd(queryDesc);
    }
    else {
        standard_ExecutorEnd(queryDesc);
    }
}

// Builds and enqueues a utility span. A non-NULL message marks it errored.
static void emitUtilitySpan(ProcessUtilityContext context, uint64 startNs, uint64 endNs, const char* commandTagName, const uint8* traceId, const uint8* parentSpanId, const uint8* utilitySpanId, const char* sqlState, const char* message) {
    Span span;

    buildUtilitySpan(&span, context, startNs, endNs, commandTagName, traceId, parentSpanId, utilitySpanId);

    if (message) {
        addErrorToSpan(&span, sqlState, message);
    }

    enqueueSpan(&span);
}

static void processUtilityHook(PlannedStmt* pstmt, const char* queryString, bool readOnlyTree, ProcessUtilityContext context, ParamListInfo params, QueryEnvironment* queryEnv, DestReceiver* dest, QueryCompletion* qc) {
    uint8 traceId[PG_EATRACE_TRACE_ID_SIZE];
    uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE];
    uint8 utilitySpanId[PG_EATRACE_SPAN_ID_SIZE];
    bool hasTraceParent = false;
    int utilitySpanIndex = -1;
    CommandTag commandTag = CMDTAG_UNKNOWN;
    const char* commandTagName = "UNKNOWN";
    uint64 startNs;

    if (!spanGenerationEnabled()) {
        if (previousProcessUtility) {
            previousProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
        }
        else {
            standard_ProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
        }

        return;
    }

    memset(traceId, 0, sizeof(traceId));
    memset(parentSpanId, 0, sizeof(parentSpanId));
    memset(utilitySpanId, 0, sizeof(utilitySpanId));

    hasTraceParent = findTraceContextForStatement(queryString, traceId, parentSpanId);
    if (hasTraceParent) {
        generateSpanId(utilitySpanId);
        utilitySpanIndex = pushActiveSpan(traceId, utilitySpanId);
    }

    if (pstmt && pstmt->utilityStmt) {
        commandTag = CreateCommandTag(pstmt->utilityStmt);
        commandTagName = GetCommandTagName(commandTag);
    }

    startNs = getCurrentUnixTime();

    PG_TRY();
    {
        if (previousProcessUtility) {
            previousProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
        }
        else {
            standard_ProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
        }
    }
    PG_CATCH();
    {
        CaughtError error;

        copyCaughtError(&error);

        popActiveSpanToIndex(utilitySpanIndex);

        if (hasTraceParent) {
            emitUtilitySpan(context, startNs, getCurrentUnixTime(), commandTagName, traceId, parentSpanId, utilitySpanId, error.sqlState, error.message);
        }

        PG_RE_THROW();
    }
    PG_END_TRY();

    popActiveSpanToIndex(utilitySpanIndex);

    if (hasTraceParent) {
        emitUtilitySpan(context, startNs, getCurrentUnixTime(), commandTagName, traceId, parentSpanId, utilitySpanId, NULL, NULL);
    }
}

// we save the previous hook in a static variable to call it later.
void _PG_init(void) {
    defineTracingGucs();
    defineWorkerGucs();
    definePlanGucs();
    defineQueueGucs();

    MarkGUCPrefixReserved("pg_eatrace");

    if (!process_shared_preload_libraries_in_progress) {
        return;
    }

    previousShmemRequestHook = shmem_request_hook;
    shmem_request_hook = shmemRequestHook;
    previousShmemStartupHook = shmem_startup_hook;
    shmem_startup_hook = shmemStartupHook;

    previousPlanner = planner_hook;
    planner_hook = plannerHook;

    previousExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = executorEndHook;

    previousExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = executorStartHook;

    previousExecutorRun = ExecutorRun_hook;
    ExecutorRun_hook = executorRunHook;

    previousExecutorFinish = ExecutorFinish_hook;
    ExecutorFinish_hook = executorFinishHook;

    previousProcessUtility = ProcessUtility_hook;
    ProcessUtility_hook = processUtilityHook;

    RegisterXactCallback(xactEndCallback, NULL);

    registerWorker();
}
