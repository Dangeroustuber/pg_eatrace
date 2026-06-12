#include "postgres.h"

#include "miscadmin.h"
#include "utils/guc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

#include "pg_eatrace_queue.h"

#define PG_EATRACE_LWLOCK_TRANCHE "pg_eatrace"
#define PG_EATRACE_SHMEM_NAME "pg_eatrace shared state"

static int maxSpans = 5000;

typedef struct EatraceSharedState {
    LWLock* lock;
    int     capacity;
    int     writeIndex;
    int     readIndex;
    int     count;
    uint64  droppedCount;
    uint64  dequeuedCount;
    uint64  exportedCount;
    uint64  exportFailureCount;
    long    lastHttpStatusCode;
    char    lastErrorMessage[PG_EATRACE_HTTP_ERROR_MESSAGE_SIZE];
    TimestampTz lastSuccessfulExportAt;
    TimestampTz lastFailedExportAt;
    // Span ring buffer follows this struct in shared memory; access via sharedSpans().
} EatraceSharedState;

static EatraceSharedState* sharedState = NULL;

static Span* sharedSpans(void) {
    return (Span*)((char*)sharedState + MAXALIGN(sizeof(EatraceSharedState)));
}

static Size sharedStateSize(void) {
    return MAXALIGN(sizeof(EatraceSharedState)) + (Size)maxSpans * sizeof(Span);
}

void defineQueueGucs(void) {
    DefineCustomIntVariable("pg_eatrace.max_spans",
        "Maximum number of spans to buffer in shared memory. Requires restart.",
        NULL,
        &maxSpans,
        5000,
        1,
        10000,
        PGC_POSTMASTER,
        0,
        NULL,
        NULL,
        NULL);
}

void requestQueueShmem(void) {
    RequestAddinShmemSpace(sharedStateSize());
    RequestNamedLWLockTranche(PG_EATRACE_LWLOCK_TRANCHE, 1);
}

void initQueueShmem(void) {
    bool found;

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    sharedState = ShmemInitStruct(PG_EATRACE_SHMEM_NAME, sharedStateSize(), &found);

    if (!found) {
        memset(sharedState, 0, sharedStateSize());
        sharedState->capacity = maxSpans;
        sharedState->lock = &(GetNamedLWLockTranche(PG_EATRACE_LWLOCK_TRANCHE)[0].lock);
    }

    LWLockRelease(AddinShmemInitLock);
}

bool queueIsReady(void) {
    return sharedState != NULL;
}

bool enqueueSpan(Span* span) {
    bool enqueued = false;

    if (!sharedState) {
        return false;
    }

    LWLockAcquire(sharedState->lock, LW_EXCLUSIVE);

    if (sharedState->count >= sharedState->capacity) {
        sharedState->droppedCount++;
    }
    else {
        sharedSpans()[sharedState->writeIndex] = *span;
        sharedState->writeIndex = (sharedState->writeIndex + 1) % sharedState->capacity;
        sharedState->count++;
        enqueued = true;
    }

    LWLockRelease(sharedState->lock);

    return enqueued;
}

// Copies up to maxCount spans out of the ring under one lock acquisition.
int dequeueSpanBatch(Span* spans, int maxCount) {
    int dequeued = 0;

    if (!sharedState || maxCount <= 0) {
        return 0;
    }

    LWLockAcquire(sharedState->lock, LW_EXCLUSIVE);

    while (dequeued < maxCount && sharedState->count > 0) {
        spans[dequeued++] = sharedSpans()[sharedState->readIndex];
        sharedState->readIndex = (sharedState->readIndex + 1) % sharedState->capacity;
        sharedState->count--;
        sharedState->dequeuedCount++;
    }

    LWLockRelease(sharedState->lock);

    return dequeued;
}

bool dequeueSpan(Span* span) {
    return dequeueSpanBatch(span, 1) == 1;
}

bool getQueueStatus(QueueStatus* status) {
    if (!status) {
        return false;
    }

    memset(status, 0, sizeof(QueueStatus));

    if (!sharedState) {
        return false;
    }

    LWLockAcquire(sharedState->lock, LW_SHARED);

    status->queueCapacity = sharedState->capacity;
    status->queueDepth = sharedState->count;
    status->droppedSpans = sharedState->droppedCount;
    status->dequeuedSpans = sharedState->dequeuedCount;
    status->exportedSpans = sharedState->exportedCount;
    status->exportFailures = sharedState->exportFailureCount;
    status->lastHttpStatusCode = sharedState->lastHttpStatusCode;
    strlcpy(status->lastErrorMessage, sharedState->lastErrorMessage, sizeof(status->lastErrorMessage));
    status->lastSuccessfulExportAt = sharedState->lastSuccessfulExportAt;
    status->lastFailedExportAt = sharedState->lastFailedExportAt;

    LWLockRelease(sharedState->lock);

    return true;
}

static void recordLastHttpResult(HttpResult* requestResult) {
    if (!requestResult) {
        return;
    }

    sharedState->lastHttpStatusCode = requestResult->httpStatusCode;
    strlcpy(sharedState->lastErrorMessage, requestResult->errorMessage, sizeof(sharedState->lastErrorMessage));
}

void recordExportSuccess(HttpResult* requestResult, int spanCount) {
    if (!sharedState) {
        return;
    }

    LWLockAcquire(sharedState->lock, LW_EXCLUSIVE);

    sharedState->exportedCount += spanCount;
    recordLastHttpResult(requestResult);
    sharedState->lastSuccessfulExportAt = GetCurrentTimestamp();

    LWLockRelease(sharedState->lock);
}

void recordExportFailure(HttpResult* requestResult) {
    if (!sharedState) {
        return;
    }

    LWLockAcquire(sharedState->lock, LW_EXCLUSIVE);

    sharedState->exportFailureCount++;
    recordLastHttpResult(requestResult);
    sharedState->lastFailedExportAt = GetCurrentTimestamp();

    LWLockRelease(sharedState->lock);
}
