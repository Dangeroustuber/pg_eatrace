#include "postgres.h"

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_classes.h"

#include "pg_eatrace_http.h"
#include "pg_eatrace_otlp_json.h"
#include "pg_eatrace_queue.h"
#include "pg_eatrace_span.h"
#include "pg_eatrace_trace_context.h"
#include "pg_eatrace_worker.h"

// Mirrors the bounded connect/total times previously configured on libcurl.
#define PG_EATRACE_HTTP_CONNECT_TIMEOUT_MS 2000
#define PG_EATRACE_HTTP_TIMEOUT_MS 5000

static char* collectorUrl = NULL;
static int   exportIntervalMs = 1000;
static int   maxExportBatch = 500;

static bool checkCollectorUrl(char** newValue, void** extra, GucSource source) {
    char errorMessage[PG_EATRACE_HTTP_ERROR_MESSAGE_SIZE];

    // Empty disables export; the worker logs drained spans instead.
    if (*newValue == NULL || (*newValue)[0] == '\0') {
        return true;
    }

    if (!httpUrlIsValid(*newValue, errorMessage, sizeof(errorMessage))) {
        GUC_check_errdetail("%s", errorMessage);
        return false;
    }

    return true;
}

void defineWorkerGucs(void) {
    DefineCustomStringVariable("pg_eatrace.collector_url",
        "OTLP/HTTP collector URL for span export (plain http:// only).",
        NULL,
        &collectorUrl,
        NULL,
        PGC_SIGHUP,
        0,
        checkCollectorUrl,
        NULL,
        NULL);
    DefineCustomIntVariable("pg_eatrace.export_interval_ms",
        "How often the background worker exports buffered spans, in milliseconds.",
        NULL,
        &exportIntervalMs,
        1000,
        100,
        60000,
        PGC_SIGHUP,
        0,
        NULL,
        NULL,
        NULL);
    DefineCustomIntVariable("pg_eatrace.max_export_batch",
        "Maximum number of spans the background worker sends in a single OTLP request.",
        NULL,
        &maxExportBatch,
        500,
        1,
        10000,
        PGC_SIGHUP,
        0,
        NULL,
        NULL,
        NULL);
}

void registerWorker(void) {
    BackgroundWorker worker = {
        .bgw_flags = BGWORKER_SHMEM_ACCESS,
        .bgw_start_time = BgWorkerStart_ConsistentState,
        .bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL,
    };

    snprintf(worker.bgw_name, BGW_MAXLEN, "pg_eatrace worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "pg_eatrace");
    snprintf(worker.bgw_library_name, MAXPGPATH, "pg_eatrace");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "workerMain");

    RegisterBackgroundWorker(&worker);
}

static void logSpan(Span* span) {
    char traceIdHex[PG_EATRACE_TRACE_ID_HEX_SIZE];
    char spanIdHex[PG_EATRACE_SPAN_ID_HEX_SIZE];
    char parentSpanIdHex[PG_EATRACE_SPAN_ID_HEX_SIZE];

    formatIdBytes(traceIdHex, span->traceId, PG_EATRACE_TRACE_ID_SIZE);
    formatIdBytes(spanIdHex, span->spanId, PG_EATRACE_SPAN_ID_SIZE);
    formatIdBytes(parentSpanIdHex, span->parentSpanId, PG_EATRACE_SPAN_ID_SIZE);

    elog(LOG,
        "pg_eatrace span: traceId=%s spanId=%s parentSpanId=%s name=%s type=%d"
        " statusCode=%d statusMessage=%s startTimeUnixNano=" UINT64_FORMAT " endTimeUnixNano=" UINT64_FORMAT,
        traceIdHex, spanIdHex, parentSpanIdHex, span->name, span->type,
        span->statusCode, span->statusMessage,
        span->startTimeUnixNano, span->endTimeUnixNano);
}

// Drains up to batchSize spans into the caller's buffer and exports them as one
// OTLP request. Sets *drainedAny if at least one span was pulled. Returns false
// on export failure so the caller stops draining this wake-up instead of
// hammering an unresponsive collector with the rest of the backlog.
static bool exportOneBatch(Span* spans, int batchSize, bool* drainedAny) {
    OtlpBatch batch;
    HttpResult result;
    char* json;
    int count;
    bool ok;

    count = dequeueSpanBatch(spans, batchSize);
    *drainedAny = count > 0;

    if (count == 0) {
        return true;
    }

    otlpBatchBegin(&batch);
    for (int index = 0; index < count; index++) {
        otlpBatchAddSpan(&batch, &spans[index]);
    }

    json = otlpBatchFinish(&batch);
    ok = httpPostJson(collectorUrl, json, PG_EATRACE_HTTP_CONNECT_TIMEOUT_MS, PG_EATRACE_HTTP_TIMEOUT_MS, &result);
    pfree(json);

    if (!ok) {
        recordExportFailure(&result);
        elog(WARNING, "pg_eatrace worker: export failed: %s", result.errorMessage);
    }
    else if (result.httpStatusCode < 200 || result.httpStatusCode >= 300) {
        recordExportFailure(&result);
        elog(WARNING, "pg_eatrace worker: collector returned HTTP status %ld", result.httpStatusCode);
        ok = false;
    }
    else {
        recordExportSuccess(&result, batch.spanCount);
    }

    return ok;
}

static void drainAndExportBatches(void) {
    Span* spans;
    bool drainedAny;

    if (!collectorUrl || collectorUrl[0] == '\0') {
        Span span;

        while (dequeueSpan(&span)) {
            logSpan(&span);
        }
        return;
    }

    spans = palloc(maxExportBatch * sizeof(Span));

    // Flush the backlog in bounded batches within this wake-up, but stop on the
    // first export failure so a dead collector costs one timeout, not many.
    do {
        if (!exportOneBatch(spans, maxExportBatch, &drainedAny)) {
            break;
        }
    } while (drainedAny && !ShutdownRequestPending);

    pfree(spans);
}

void workerMain(Datum mainArg) {
    MemoryContext workContext;

    (void)mainArg;

    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    BackgroundWorkerUnblockSignals();

    if (!queueIsReady()) {
        elog(WARNING, "pg_eatrace worker: shared memory is not initialized");
        proc_exit(1);
    }

    // Per-iteration scratch space: anything palloc'd while draining is wiped by
    // the reset below, so a missed pfree cannot leak for the worker's lifetime.
    workContext = AllocSetContextCreate(TopMemoryContext, "pg_eatrace worker", ALLOCSET_DEFAULT_SIZES);

    while (!ShutdownRequestPending) {
        if (ConfigReloadPending) {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        MemoryContextSwitchTo(workContext);
        drainAndExportBatches();
        MemoryContextSwitchTo(TopMemoryContext);
        MemoryContextReset(workContext);

        if (ShutdownRequestPending) {
            break;
        }

        // Skip the sleep when a reload request arrived mid-drain: the HTTP
        // client consumes latch wakeups while waiting on its socket, so only
        // the pending flag is left to tell us.
        if (!ConfigReloadPending) {
            (void)WaitLatch(MyLatch,
                WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                (long)exportIntervalMs,
                PG_WAIT_EXTENSION);
            ResetLatch(MyLatch);
        }
    }

    proc_exit(0);
}
