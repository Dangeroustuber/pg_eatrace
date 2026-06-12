#ifndef PG_EATRACE_QUEUE_H
#define PG_EATRACE_QUEUE_H

#include "postgres.h"

#include "utils/timestamp.h"

#include "pg_eatrace_http.h"
#include "pg_eatrace_span.h"

typedef struct QueueStatus {
    int queueDepth;
    int queueCapacity;
    uint64 droppedSpans;
    uint64 dequeuedSpans;
    uint64 exportedSpans;
    uint64 exportFailures;
    long lastHttpStatusCode;
    char lastErrorMessage[PG_EATRACE_HTTP_ERROR_MESSAGE_SIZE];
    TimestampTz lastSuccessfulExportAt;
    TimestampTz lastFailedExportAt;
} QueueStatus;

extern void defineQueueGucs(void);
extern void requestQueueShmem(void);
extern void initQueueShmem(void);
extern bool queueIsReady(void);
extern bool enqueueSpan(Span* span);
extern bool dequeueSpan(Span* span);
extern int dequeueSpanBatch(Span* spans, int maxCount);
extern bool getQueueStatus(QueueStatus* status);
extern void recordExportSuccess(HttpResult* requestResult, int spanCount);
extern void recordExportFailure(HttpResult* requestResult);

#endif // PG_EATRACE_QUEUE_H
