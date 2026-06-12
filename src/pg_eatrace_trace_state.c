#include "postgres.h"

#include "executor/execdesc.h"
#include "nodes/plannodes.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "pg_eatrace_trace_state.h"

static HTAB* queryStates = NULL;
static HTAB* plannerStates = NULL;

static HTAB* getQueryStateHash(void) {
    HASHCTL hashControl = {
        .keysize = sizeof(QueryDesc*),
        .entrysize = sizeof(QueryTraceState),
        .hcxt = TopMemoryContext,
    };
    MemoryContext oldContext;

    if (queryStates) {
        return queryStates;
    }

    oldContext = MemoryContextSwitchTo(TopMemoryContext);
    queryStates = hash_create("pg_eatrace query states", 32, &hashControl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    MemoryContextSwitchTo(oldContext);

    return queryStates;
}

static HTAB* getPlannerStateHash(void) {
    HASHCTL hashControl = {
        .keysize = sizeof(PlannedStmt*),
        .entrysize = sizeof(PlannerTraceState),
        .hcxt = TopMemoryContext,
    };
    MemoryContext oldContext;

    if (plannerStates) {
        return plannerStates;
    }

    oldContext = MemoryContextSwitchTo(TopMemoryContext);
    plannerStates = hash_create("pg_eatrace planner states", 32, &hashControl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    MemoryContextSwitchTo(oldContext);

    return plannerStates;
}

QueryTraceState* createQueryState(QueryDesc* queryDesc) {
    bool found;
    QueryTraceState* state;

    state = hash_search(getQueryStateHash(), &queryDesc, HASH_ENTER, &found);
    memset(state, 0, sizeof(QueryTraceState));
    state->queryDesc = queryDesc;
    state->activeSpanIndex = -1;

    return state;
}

PlannerTraceState* createPlannerState(PlannedStmt* plannedStatement) {
    bool found;
    PlannerTraceState* state;

    state = hash_search(getPlannerStateHash(), &plannedStatement, HASH_ENTER, &found);
    memset(state, 0, sizeof(PlannerTraceState));
    state->plannedStatement = plannedStatement;

    return state;
}

QueryTraceState* findQueryState(QueryDesc* queryDesc) {
    if (!queryStates) {
        return NULL;
    }

    return hash_search(queryStates, &queryDesc, HASH_FIND, NULL);
}

PlannerTraceState* findPlannerState(PlannedStmt* plannedStatement) {
    if (!plannerStates) {
        return NULL;
    }

    return hash_search(plannerStates, &plannedStatement, HASH_FIND, NULL);
}

void deleteQueryState(QueryDesc* queryDesc) {
    if (queryStates) {
        (void)hash_search(queryStates, &queryDesc, HASH_REMOVE, NULL);
    }
}

void deletePlannerState(PlannedStmt* plannedStatement) {
    if (plannerStates) {
        (void)hash_search(plannerStates, &plannedStatement, HASH_REMOVE, NULL);
    }
}

// Both hashes key by a pointer stored at the start of the entry, so the entry
// itself doubles as the key for removal. dynahash allows removing the entry a
// sequential scan just returned.
static void deleteAllEntries(HTAB* hash) {
    HASH_SEQ_STATUS scan;
    void* entry;

    hash_seq_init(&scan, hash);
    while ((entry = hash_seq_search(&scan)) != NULL) {
        (void)hash_search(hash, entry, HASH_REMOVE, NULL);
    }
}

// End-of-transaction cleanup for states whose normal executor-hook teardown
// was bypassed; their keys dangle once the owning memory contexts are reset.
void deleteAllTraceStates(void) {
    if (queryStates) {
        deleteAllEntries(queryStates);
    }

    if (plannerStates) {
        deleteAllEntries(plannerStates);
    }
}
