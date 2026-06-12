#include "postgres.h"

#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "pg_eatrace_queue.h"
#include "pg_eatrace_status.h"

PG_FUNCTION_INFO_V1(pg_eatrace_status);

Datum pg_eatrace_status(PG_FUNCTION_ARGS) {
    QueueStatus status;
    TupleDesc tupleDesc;
    Datum values[10];
    bool nulls[10] = { 0 };

    if (!getQueueStatus(&status)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                errmsg("pg_eatrace shared memory is not initialized"),
                errhint("Add pg_eatrace to shared_preload_libraries and restart PostgreSQL.")));
    }

    // The OUT parameters declared in pg_eatrace--0.0.sql are the single source
    // of truth for the result row shape.
    if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE) {
        elog(ERROR, "return type must be a row type");
    }

    tupleDesc = BlessTupleDesc(tupleDesc);

    values[0] = Int32GetDatum(status.queueDepth);
    values[1] = Int32GetDatum(status.queueCapacity);
    values[2] = Int64GetDatum((int64)status.droppedSpans);
    values[3] = Int64GetDatum((int64)status.exportedSpans);
    values[4] = Int64GetDatum((int64)status.exportFailures);
    values[5] = Int64GetDatum((int64)status.dequeuedSpans);

    if (status.lastErrorMessage[0] != '\0') {
        values[6] = CStringGetTextDatum(status.lastErrorMessage);
    }
    else {
        nulls[6] = true;
    }

    if (status.lastHttpStatusCode > 0) {
        values[7] = Int64GetDatum((int64)status.lastHttpStatusCode);
    }
    else {
        nulls[7] = true;
    }

    if (status.lastSuccessfulExportAt != 0) {
        values[8] = TimestampTzGetDatum(status.lastSuccessfulExportAt);
    }
    else {
        nulls[8] = true;
    }

    if (status.lastFailedExportAt != 0) {
        values[9] = TimestampTzGetDatum(status.lastFailedExportAt);
    }
    else {
        nulls[9] = true;
    }

    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupleDesc, values, nulls)));
}
