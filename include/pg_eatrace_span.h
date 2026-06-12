#ifndef PG_EATRACE_SPAN_H
#define PG_EATRACE_SPAN_H

#include "postgres.h"

#include "executor/instrument.h"
#include "jit/jit.h"

// W3C trace-context ids as raw bytes; hex exists only at the parse and
// serialize boundaries.
#define PG_EATRACE_TRACE_ID_SIZE 16
#define PG_EATRACE_SPAN_ID_SIZE 8
#define PG_EATRACE_SPAN_STATUS_UNSET 0
#define PG_EATRACE_SPAN_STATUS_ERROR 2

#define PG_EATRACE_NODE_TYPE_NAME_SIZE 40
#define PG_EATRACE_COMMAND_TAG_SIZE 64

// Span payloads are typed per family. The OTLP serializer expands these into
// key/value attributes at export time, so attribute keys exist only as string
// literals in the serializer, never as data copied through shared memory.
typedef enum SpanType {
    PG_EATRACE_SPAN_QUERY,
    PG_EATRACE_SPAN_PLANNING,
    PG_EATRACE_SPAN_UTILITY,
    PG_EATRACE_SPAN_PLAN_NODE,
    PG_EATRACE_SPAN_WORKER
} SpanType;

typedef struct QuerySpanData {
    // Set on the planner-error path, where resource usage never exists.
    char commandTag[PG_EATRACE_COMMAND_TAG_SIZE];
    bool hasResourceUsage;
    BufferUsage bufferUsage;
    WalUsage walUsage;
    bool hasJitFlags;
    int jitFlags;
    bool hasJit;
    JitInstrumentation jit;
    bool hasWorkerJit;
    JitInstrumentation workerJit;
} QuerySpanData;

typedef struct PlannerSpanData {
    char commandTag[PG_EATRACE_COMMAND_TAG_SIZE];
    int cursorOptions;
    // False when planning failed before producing a PlannedStmt.
    bool hasPlanStats;
    char topNodeType[PG_EATRACE_NODE_TYPE_NAME_SIZE];
    double startupCost;
    double totalCost;
    double estimatedRows;
    int estimatedWidth;
    int nodeCount;
    int maxDepth;
    int scanCount;
    int joinCount;
    bool parallelModeNeeded;
    int parallelNodeCount;
    int workersPlanned;
    int relationRteCount;
    int subqueryRteCount;
    int functionRteCount;
    int jitFlags;
} PlannerSpanData;

typedef struct UtilitySpanData {
    char commandTag[PG_EATRACE_COMMAND_TAG_SIZE];
    char context[24];
} UtilitySpanData;

typedef struct PlanNodeSpanData {
    char nodeType[PG_EATRACE_NODE_TYPE_NAME_SIZE];
    int nodeId;
    double rows;
    double loops;
    double totalSeconds;
    double startupSeconds;
    double estimatedRows;
    double startupCost;
    double totalCost;
    bool parallelAware;
    bool parallelSafe;
    char relationName[NAMEDATALEN];
    char indexName[NAMEDATALEN];
    char joinType[16];
    char modifyOperation[8];
    BufferUsage bufferUsage;
    WalUsage walUsage;
    bool hasGatherInfo;
    bool hasSingleCopy;
    bool singleCopy;
    int workersPlanned;
    int workersLaunched;
    bool hasWorkerSummary;
    int workerCount;
    int activeWorkerCount;
    double workerRows;
    double workerLoops;
    double workerTotalSeconds;
    double workerMaxSeconds;
} PlanNodeSpanData;

typedef struct WorkerSpanData {
    char nodeType[PG_EATRACE_NODE_TYPE_NAME_SIZE];
    int nodeId;
    int workerIndex;
    double rows;
    double loops;
    double totalSeconds;
    double startupSeconds;
    BufferUsage bufferUsage;
    WalUsage walUsage;
} WorkerSpanData;

typedef struct Span {
    uint8 traceId[PG_EATRACE_TRACE_ID_SIZE];
    uint8 spanId[PG_EATRACE_SPAN_ID_SIZE];
    uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE];
    SpanType type;
    int statusCode;
    char sqlState[6];
    char name[64];
    char statusMessage[256];
    uint64 startTimeUnixNano;
    uint64 endTimeUnixNano;
    union {
        QuerySpanData query;
        PlannerSpanData planner;
        UtilitySpanData utility;
        PlanNodeSpanData planNode;
        WorkerSpanData worker;
    } data;
} Span;

// A NULL spanId generates a fresh random id; NULL traceId/parentSpanId zero the field.
extern void initSpan(Span* span, SpanType type, const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], const uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], const uint8 spanId[PG_EATRACE_SPAN_ID_SIZE], const char* name, uint64 startNs, uint64 endNs);
extern void addErrorToSpan(Span* span, const char* sqlState, const char* message);

#endif // PG_EATRACE_SPAN_H
