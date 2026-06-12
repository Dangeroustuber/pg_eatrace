#include "postgres.h"

#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "tcop/cmdtag.h"
#include "tcop/utility.h"

#include "pg_eatrace_plan.h"
#include "pg_eatrace_planner.h"
#include "pg_eatrace_trace_context.h"

typedef struct PlannerPlanStats {
    int nodeCount;
    int maxDepth;
    int scanCount;
    int joinCount;
    int parallelNodeCount;
    int workersPlanned;
} PlannerPlanStats;

static bool isPlannerScanNode(Plan* plan) {
    switch (nodeTag(plan)) {
    case T_SeqScan:
    case T_SampleScan:
    case T_IndexScan:
    case T_IndexOnlyScan:
    case T_BitmapIndexScan:
    case T_BitmapHeapScan:
    case T_TidScan:
    case T_TidRangeScan:
    case T_SubqueryScan:
    case T_FunctionScan:
    case T_TableFuncScan:
    case T_ValuesScan:
    case T_CteScan:
    case T_NamedTuplestoreScan:
    case T_WorkTableScan:
    case T_ForeignScan:
    case T_CustomScan:
        return true;
    default:
        return false;
    }
}

static bool isPlannerJoinNode(Plan* plan) {
    switch (nodeTag(plan)) {
    case T_NestLoop:
    case T_MergeJoin:
    case T_HashJoin:
        return true;
    default:
        return false;
    }
}

static void collectPlanStatsFromList(List* plans, PlannerPlanStats* stats, int depth);

static void collectPlanStats(Plan* plan, PlannerPlanStats* stats, int depth) {
    if (!plan || !stats) {
        return;
    }

    check_stack_depth();

    stats->nodeCount++;
    stats->maxDepth = Max(stats->maxDepth, depth);

    if (isPlannerScanNode(plan)) {
        stats->scanCount++;
    }

    if (isPlannerJoinNode(plan)) {
        stats->joinCount++;
    }

    if (plan->parallel_aware) {
        stats->parallelNodeCount++;
    }

    switch (nodeTag(plan)) {
    case T_Gather:
        stats->parallelNodeCount++;
        stats->workersPlanned += ((Gather*)plan)->num_workers;
        break;
    case T_GatherMerge:
        stats->parallelNodeCount++;
        stats->workersPlanned += ((GatherMerge*)plan)->num_workers;
        break;
    default:
        break;
    }

    collectPlanStats(outerPlan(plan), stats, depth + 1);
    collectPlanStats(innerPlan(plan), stats, depth + 1);

    switch (nodeTag(plan)) {
    case T_Append:
        collectPlanStatsFromList(((Append*)plan)->appendplans, stats, depth + 1);
        break;
    case T_MergeAppend:
        collectPlanStatsFromList(((MergeAppend*)plan)->mergeplans, stats, depth + 1);
        break;
    case T_BitmapAnd:
        collectPlanStatsFromList(((BitmapAnd*)plan)->bitmapplans, stats, depth + 1);
        break;
    case T_BitmapOr:
        collectPlanStatsFromList(((BitmapOr*)plan)->bitmapplans, stats, depth + 1);
        break;
    case T_SubqueryScan:
        collectPlanStats(((SubqueryScan*)plan)->subplan, stats, depth + 1);
        break;
    case T_CustomScan:
        collectPlanStatsFromList(((CustomScan*)plan)->custom_plans, stats, depth + 1);
        break;
    default:
        break;
    }
}

static void collectPlanStatsFromList(List* plans, PlannerPlanStats* stats, int depth) {
    ListCell* cell;

    foreach(cell, plans) {
        collectPlanStats((Plan*)lfirst(cell), stats, depth);
    }
}

static int countRteKind(List* rtable, RTEKind kind) {
    int count = 0;
    ListCell* cell;

    foreach(cell, rtable) {
        RangeTblEntry* rte = (RangeTblEntry*)lfirst(cell);

        if (rte->rtekind == kind) {
            count++;
        }
    }

    return count;
}

static void fillPlannerPlanStats(PlannerSpanData* data, PlannedStmt* plannedStatement) {
    Plan* plan;
    PlannerPlanStats stats = {
        .nodeCount = 0,
        .maxDepth = 0,
        .scanCount = 0,
        .joinCount = 0,
        .parallelNodeCount = 0,
        .workersPlanned = 0,
    };

    if (!plannedStatement || !plannedStatement->planTree) {
        return;
    }

    plan = plannedStatement->planTree;
    collectPlanStats(plan, &stats, 1);

    data->hasPlanStats = true;
    strlcpy(data->topNodeType, planNodeTypeName(plan), sizeof(data->topNodeType));
    data->startupCost = plan->startup_cost;
    data->totalCost = plan->total_cost;
    data->estimatedRows = plan->plan_rows;
    data->estimatedWidth = plan->plan_width;
    data->nodeCount = stats.nodeCount;
    data->maxDepth = stats.maxDepth;
    data->scanCount = stats.scanCount;
    data->joinCount = stats.joinCount;
    data->parallelModeNeeded = plannedStatement->parallelModeNeeded;
    data->parallelNodeCount = stats.parallelNodeCount;
    data->workersPlanned = stats.workersPlanned;
    data->relationRteCount = countRteKind(plannedStatement->rtable, RTE_RELATION);
    data->subqueryRteCount = countRteKind(plannedStatement->rtable, RTE_SUBQUERY);
    data->functionRteCount = countRteKind(plannedStatement->rtable, RTE_FUNCTION);
    data->jitFlags = plannedStatement->jitFlags;
}

void buildPlannerSpan(Span* span, Query* parse, PlannedStmt* plannedStatement, int cursorOptions, uint64 startNs, uint64 endNs, const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], const uint8 querySpanId[PG_EATRACE_SPAN_ID_SIZE]) {
    const char* commandTagName = "UNKNOWN";

    if (parse) {
        commandTagName = GetCommandTagName(CreateCommandTag((Node*)parse));
    }

    initSpan(span, PG_EATRACE_SPAN_PLANNING, traceId, querySpanId, NULL, "postgres.planning", startNs, endNs);

    strlcpy(span->data.planner.commandTag, commandTagName, sizeof(span->data.planner.commandTag));
    span->data.planner.cursorOptions = cursorOptions;
    fillPlannerPlanStats(&span->data.planner, plannedStatement);
}
