#include "postgres.h"

#include "executor/instrument.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "pg_eatrace_plan.h"
#include "pg_eatrace_queue.h"
#include "pg_eatrace_span.h"
#include "pg_eatrace_trace_context.h"

typedef struct PlanNodeSpanContext {
    Span* querySpan;
    const uint8* parentSpanId;
    uint64 queryStartNs;
    uint64 queryEndNs;
} PlanNodeSpanContext;

typedef struct PlanNodeMetadata {
    const char* relationName;
    char* indexName;
    const char* joinType;
    const char* modifyOperation;
    char spanName[64];
} PlanNodeMetadata;

bool parallelWorkerSpans = false;

void definePlanGucs(void) {
    DefineCustomBoolVariable("pg_eatrace.parallel_worker_spans", "Emit per-worker child spans for parallel plan nodes.", NULL, &parallelWorkerSpans, false, PGC_SUSET, 0, NULL, NULL, NULL);
}

const char* planNodeTypeName(Plan* plan) {
    if (!plan) {
        return "Unknown Plan Node";
    }

    switch (nodeTag(plan)) {
    case T_Result:
        return "Result";
    case T_ProjectSet:
        return "ProjectSet";
    case T_ModifyTable:
        return "ModifyTable";
    case T_Append:
        return "Append";
    case T_MergeAppend:
        return "Merge Append";
    case T_RecursiveUnion:
        return "Recursive Union";
    case T_BitmapAnd:
        return "BitmapAnd";
    case T_BitmapOr:
        return "BitmapOr";
    case T_NestLoop:
        return "Nested Loop";
    case T_MergeJoin:
        return "Merge Join";
    case T_HashJoin:
        return "Hash Join";
    case T_SeqScan:
        return "Seq Scan";
    case T_SampleScan:
        return "Sample Scan";
    case T_Gather:
        return "Gather";
    case T_GatherMerge:
        return "Gather Merge";
    case T_IndexScan:
        return "Index Scan";
    case T_IndexOnlyScan:
        return "Index Only Scan";
    case T_BitmapIndexScan:
        return "Bitmap Index Scan";
    case T_BitmapHeapScan:
        return "Bitmap Heap Scan";
    case T_TidScan:
        return "Tid Scan";
    case T_TidRangeScan:
        return "Tid Range Scan";
    case T_SubqueryScan:
        return "Subquery Scan";
    case T_FunctionScan:
        return "Function Scan";
    case T_TableFuncScan:
        return "Table Function Scan";
    case T_ValuesScan:
        return "Values Scan";
    case T_CteScan:
        return "CTE Scan";
    case T_NamedTuplestoreScan:
        return "Named Tuplestore Scan";
    case T_WorkTableScan:
        return "WorkTable Scan";
    case T_ForeignScan:
        return "Foreign Scan";
    case T_CustomScan:
        return "Custom Scan";
    case T_Material:
        return "Materialize";
    case T_Memoize:
        return "Memoize";
    case T_Sort:
        return "Sort";
    case T_IncrementalSort:
        return "Incremental Sort";
    case T_Group:
        return "Group";
    case T_Agg:
        return "Aggregate";
    case T_WindowAgg:
        return "WindowAgg";
    case T_Unique:
        return "Unique";
    case T_SetOp:
        return "SetOp";
    case T_LockRows:
        return "LockRows";
    case T_Limit:
        return "Limit";
    case T_Hash:
        return "Hash";
    default:
        return "Unknown Plan Node";
    }
}

static uint64 planNodeStartNs(PlanNodeSpanContext* context, Instrumentation* instrument) {
    uint64 durationNs = secondsToNanos(instrument->total);

    if (durationNs == 0 || context->queryEndNs < durationNs) {
        return context->queryEndNs;
    }

    if (context->queryEndNs - durationNs < context->queryStartNs) {
        return context->queryStartNs;
    }

    return context->queryEndNs - durationNs;
}

static const char* joinTypeName(JoinType joinType) {
    switch (joinType) {
    case JOIN_INNER:
        return "inner";
    case JOIN_LEFT:
        return "left";
    case JOIN_FULL:
        return "full";
    case JOIN_RIGHT:
        return "right";
    case JOIN_SEMI:
        return "semi";
    case JOIN_ANTI:
        return "anti";
    case JOIN_RIGHT_SEMI:
        return "right_semi";
    case JOIN_RIGHT_ANTI:
        return "right_anti";
    default:
        return NULL;
    }
}

static const char* commandTypeName(CmdType commandType) {
    switch (commandType) {
    case CMD_INSERT:
        return "insert";
    case CMD_UPDATE:
        return "update";
    case CMD_DELETE:
        return "delete";
    case CMD_MERGE:
        return "merge";
    default:
        return NULL;
    }
}

static bool isScanPlanNode(PlanState* planstate) {
    if (!planstate || !planstate->plan) {
        return false;
    }

    switch (nodeTag(planstate->plan)) {
    case T_SeqScan:
    case T_SampleScan:
    case T_IndexScan:
    case T_IndexOnlyScan:
    case T_BitmapIndexScan:
    case T_BitmapHeapScan:
    case T_TidScan:
    case T_TidRangeScan:
        return true;
    default:
        return false;
    }
}

static const char* scanRelationName(PlanState* planstate) {
    ScanState* scanstate;

    if (!isScanPlanNode(planstate)) {
        return NULL;
    }

    scanstate = (ScanState*)planstate;
    if (!scanstate->ss_currentRelation) {
        return NULL;
    }

    return RelationGetRelationName(scanstate->ss_currentRelation);
}

static Oid planNodeIndexOid(Plan* plan) {
    switch (nodeTag(plan)) {
    case T_IndexScan:
        return ((IndexScan*)plan)->indexid;
    case T_IndexOnlyScan:
        return ((IndexOnlyScan*)plan)->indexid;
    case T_BitmapIndexScan:
        return ((BitmapIndexScan*)plan)->indexid;
    default:
        return InvalidOid;
    }
}

static void buildPlanNodeMetadata(PlanState* planstate, const char* nodeName, PlanNodeMetadata* metadata) {
    Oid indexOid;

    memset(metadata, 0, sizeof(PlanNodeMetadata));
    strlcpy(metadata->spanName, nodeName, sizeof(metadata->spanName));

    metadata->relationName = scanRelationName(planstate);

    indexOid = planNodeIndexOid(planstate->plan);
    if (OidIsValid(indexOid)) {
        metadata->indexName = get_rel_name(indexOid);
    }

    switch (nodeTag(planstate->plan)) {
    case T_ModifyTable:
        metadata->modifyOperation = commandTypeName(((ModifyTable*)planstate->plan)->operation);
        break;
    case T_NestLoop:
    case T_MergeJoin:
    case T_HashJoin:
        metadata->joinType = joinTypeName(((Join*)planstate->plan)->jointype);
        break;
    default:
        break;
    }

    if (metadata->indexName && metadata->relationName) {
        snprintf(metadata->spanName, sizeof(metadata->spanName), "%s %s on %s", nodeName, metadata->indexName, metadata->relationName);
    }
    else if (metadata->indexName) {
        snprintf(metadata->spanName, sizeof(metadata->spanName), "%s %s", nodeName, metadata->indexName);
    }
    else if (metadata->joinType) {
        snprintf(metadata->spanName, sizeof(metadata->spanName), "%s %s", nodeName, metadata->joinType);
    }
    else if (metadata->modifyOperation) {
        snprintf(metadata->spanName, sizeof(metadata->spanName), "%s %s", nodeName, metadata->modifyOperation);
    }
    else if (metadata->relationName) {
        snprintf(metadata->spanName, sizeof(metadata->spanName), "%s %s", nodeName, metadata->relationName);
    }
}

static void cleanupPlanNodeMetadata(PlanNodeMetadata* metadata) {
    if (metadata->indexName) {
        pfree(metadata->indexName);
    }
}

static void fillGatherInfo(PlanNodeSpanData* data, PlanState* planstate) {
    switch (nodeTag(planstate->plan)) {
    case T_Gather:
    {
        Gather* gather = (Gather*)planstate->plan;
        GatherState* gatherState = (GatherState*)planstate;

        data->hasGatherInfo = true;
        data->workersPlanned = gather->num_workers;
        data->workersLaunched = gatherState->nworkers_launched;
        data->hasSingleCopy = true;
        data->singleCopy = gather->single_copy;
        break;
    }
    case T_GatherMerge:
    {
        GatherMerge* gatherMerge = (GatherMerge*)planstate->plan;
        GatherMergeState* gatherMergeState = (GatherMergeState*)planstate;

        data->hasGatherInfo = true;
        data->workersPlanned = gatherMerge->num_workers;
        data->workersLaunched = gatherMergeState->nworkers_launched;
        break;
    }
    default:
        break;
    }
}

static void fillParallelWorkerSummary(PlanNodeSpanData* data, PlanState* planstate) {
    WorkerInstrumentation* workerInstrumentation;
    int activeWorkerCount = 0;
    double workerRows = 0.0;
    double workerLoops = 0.0;
    double workerTotalSeconds = 0.0;
    double workerMaxSeconds = 0.0;

    workerInstrumentation = planstate->worker_instrument;
    if (!workerInstrumentation || workerInstrumentation->num_workers <= 0) {
        return;
    }

    for (int index = 0; index < workerInstrumentation->num_workers; index++) {
        Instrumentation* workerInstrument = &workerInstrumentation->instrument[index];

        if (workerInstrument->nloops <= 0.0) {
            continue;
        }

        activeWorkerCount++;
        workerRows += workerInstrument->ntuples;
        workerLoops += workerInstrument->nloops;
        workerTotalSeconds += workerInstrument->total;

        if (workerInstrument->total > workerMaxSeconds) {
            workerMaxSeconds = workerInstrument->total;
        }
    }

    data->hasWorkerSummary = true;
    data->workerCount = workerInstrumentation->num_workers;
    data->activeWorkerCount = activeWorkerCount;
    data->workerRows = workerRows;
    data->workerLoops = workerLoops;
    data->workerTotalSeconds = workerTotalSeconds;
    data->workerMaxSeconds = workerMaxSeconds;
}

static void fillPlanNodeData(PlanNodeSpanData* data, PlanState* planstate, Instrumentation* instrument, PlanNodeMetadata* metadata, const char* nodeName) {
    Plan* plan = planstate->plan;

    strlcpy(data->nodeType, nodeName, sizeof(data->nodeType));
    data->nodeId = plan->plan_node_id;
    data->rows = instrument->ntuples;
    data->loops = instrument->nloops;
    data->totalSeconds = instrument->total;
    data->startupSeconds = instrument->startup;
    data->estimatedRows = plan->plan_rows;
    data->startupCost = plan->startup_cost;
    data->totalCost = plan->total_cost;
    data->parallelAware = plan->parallel_aware;
    data->parallelSafe = plan->parallel_safe;

    if (metadata->relationName) {
        strlcpy(data->relationName, metadata->relationName, sizeof(data->relationName));
    }

    if (metadata->indexName) {
        strlcpy(data->indexName, metadata->indexName, sizeof(data->indexName));
    }

    if (metadata->joinType) {
        strlcpy(data->joinType, metadata->joinType, sizeof(data->joinType));
    }

    if (metadata->modifyOperation) {
        strlcpy(data->modifyOperation, metadata->modifyOperation, sizeof(data->modifyOperation));
    }

    data->bufferUsage = instrument->bufusage;
    data->walUsage = instrument->walusage;

    fillGatherInfo(data, planstate);
    fillParallelWorkerSummary(data, planstate);
}

static void enqueueParallelWorkerSpans(PlanState* planstate, PlanNodeSpanContext* context, Span* planNodeSpan, const char* nodeName) {
    WorkerInstrumentation* workerInstrumentation;

    workerInstrumentation = planstate->worker_instrument;
    if (!parallelWorkerSpans || !workerInstrumentation || workerInstrumentation->num_workers <= 0) {
        return;
    }

    for (int i = 0; i < workerInstrumentation->num_workers; i++) {
        Instrumentation* workerInstrument = &workerInstrumentation->instrument[i];
        Span workerSpan;
        WorkerSpanData* data;
        char workerSpanName[64];
        uint64 startNs;

        if (workerInstrument->nloops <= 0.0) {
            continue;
        }

        snprintf(workerSpanName, sizeof(workerSpanName), "%s worker %d", nodeName, i);
        startNs = planNodeStartNs(context, workerInstrument);

        initSpan(&workerSpan, PG_EATRACE_SPAN_WORKER, planNodeSpan->traceId, planNodeSpan->spanId, NULL, workerSpanName, startNs, context->queryEndNs);

        data = &workerSpan.data.worker;
        strlcpy(data->nodeType, nodeName, sizeof(data->nodeType));
        data->nodeId = planstate->plan->plan_node_id;
        data->workerIndex = i;
        data->rows = workerInstrument->ntuples;
        data->loops = workerInstrument->nloops;
        data->totalSeconds = workerInstrument->total;
        data->startupSeconds = workerInstrument->startup;
        data->bufferUsage = workerInstrument->bufusage;
        data->walUsage = workerInstrument->walusage;

        enqueueSpan(&workerSpan);
    }
}

static void enqueuePlanNodeSpan(PlanState* planstate, PlanNodeSpanContext* context, uint8 childParentOut[PG_EATRACE_SPAN_ID_SIZE]) {
    Instrumentation* instrument;
    Span span;
    const char* nodeName;
    PlanNodeMetadata metadata;
    uint64 startNs;

    if (!context || !context->querySpan) {
        return;
    }

    // Default: a node we skip passes its own parent down, collapsing the level
    // so its children attach to the nearest emitted ancestor instead of orphaning.
    memcpy(childParentOut, context->parentSpanId, PG_EATRACE_SPAN_ID_SIZE);

    if (!planstate || !planstate->plan) {
        return;
    }

    instrument = planstate->instrument;
    if (!instrument) {
        return;
    }

    InstrEndLoop(instrument);
    if (instrument->nloops <= 0.0) {
        return;
    }

    nodeName = planNodeTypeName(planstate->plan);
    buildPlanNodeMetadata(planstate, nodeName, &metadata);
    startNs = planNodeStartNs(context, instrument);

    initSpan(&span, PG_EATRACE_SPAN_PLAN_NODE, context->querySpan->traceId, context->parentSpanId, NULL, metadata.spanName, startNs, context->queryEndNs);

    fillPlanNodeData(&span.data.planNode, planstate, instrument, &metadata, nodeName);

    enqueueSpan(&span);
    enqueueParallelWorkerSpans(planstate, context, &span, nodeName);
    cleanupPlanNodeMetadata(&metadata);

    // This node emitted a span, so its children nest beneath it.
    memcpy(childParentOut, span.spanId, PG_EATRACE_SPAN_ID_SIZE);
}

static bool planNodeSpanWalker(PlanState* planstate, PlanNodeSpanContext* context) {
    PlanNodeSpanContext childContext = *context;
    uint8 childParent[PG_EATRACE_SPAN_ID_SIZE];

    enqueuePlanNodeSpan(planstate, context, childParent);
    childContext.parentSpanId = childParent;

    return planstate_tree_walker(planstate, planNodeSpanWalker, &childContext);
}

void enqueuePlanNodeSpans(QueryDesc* queryDesc, Span* querySpan, uint64 queryStartNs, uint64 queryEndNs) {
    PlanNodeSpanContext context = {
        .querySpan = querySpan,
        .parentSpanId = querySpan ? querySpan->spanId : NULL,
        .queryStartNs = queryStartNs,
        .queryEndNs = queryEndNs,
    };
    uint8 rootChildParent[PG_EATRACE_SPAN_ID_SIZE];
    PlanNodeSpanContext childContext;

    if (!queryDesc || !queryDesc->planstate || !querySpan) {
        return;
    }

    // Root plan node parents to the query span; its children parent to it.
    enqueuePlanNodeSpan(queryDesc->planstate, &context, rootChildParent);

    childContext = context;
    childContext.parentSpanId = rootChildParent;
    (void)planstate_tree_walker(queryDesc->planstate, planNodeSpanWalker, &childContext);
}
