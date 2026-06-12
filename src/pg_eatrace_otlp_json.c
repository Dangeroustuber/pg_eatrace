#include "postgres.h"

#include "lib/stringinfo.h"

#include "pg_eatrace_otlp_json.h"
#include "pg_eatrace_trace_context.h"

#include <math.h>

// Append str as a JSON string value — no surrounding quotes.
// Escapes the characters required by RFC 8259.
static void appendToJsonString(StringInfo builder, const char* stringValue) {
    for (const char* currentChar = stringValue; *currentChar; currentChar++) {
        switch (*currentChar) {
        case '"':
            appendStringInfoString(builder, "\\\"");
            break;
        case '\\':
            appendStringInfoString(builder, "\\\\");
            break;
        case '\n':
            appendStringInfoString(builder, "\\n");
            break;
        case '\r':
            appendStringInfoString(builder, "\\r");
            break;
        case '\t':
            appendStringInfoString(builder, "\\t");
            break;
        default:
            if ((unsigned char)*currentChar < 0x20) {
                appendStringInfo(builder, "\\u%04x", (unsigned char)*currentChar);
            }
            else {
                appendStringInfoChar(builder, *currentChar);
            }
        }
    }
}

static void appendIdHex(StringInfo builder, const uint8* bytes, int length) {
    char hex[PG_EATRACE_TRACE_ID_HEX_SIZE];

    formatIdBytes(hex, bytes, length);
    appendStringInfoString(builder, hex);
}

// Incrementally writes the elements of one span's attributes array.
typedef struct AttributeWriter {
    StringInfo builder;
    bool first;
} AttributeWriter;

static void beginAttribute(AttributeWriter* writer, const char* key) {
    if (!writer->first) {
        appendStringInfoChar(writer->builder, ',');
    }
    writer->first = false;

    appendStringInfoString(writer->builder, "{\"key\":\"");
    appendToJsonString(writer->builder, key);
    appendStringInfoString(writer->builder, "\",\"value\":{");
}

static void endAttribute(AttributeWriter* writer) {
    appendStringInfoString(writer->builder, "}}");
}

static void writeStringAttribute(AttributeWriter* writer, const char* key, const char* value) {
    beginAttribute(writer, key);
    appendStringInfoString(writer->builder, "\"stringValue\":\"");
    appendToJsonString(writer->builder, value);
    appendStringInfoChar(writer->builder, '"');
    endAttribute(writer);
}

// Emits the attribute only when the value is a non-empty string.
static void writeOptionalStringAttribute(AttributeWriter* writer, const char* key, const char* value) {
    if (value[0] != '\0') {
        writeStringAttribute(writer, key, value);
    }
}

static void writeIntAttribute(AttributeWriter* writer, const char* key, int64 value) {
    beginAttribute(writer, key);
    appendStringInfo(writer->builder, "\"intValue\":\"" INT64_FORMAT "\"", value);
    endAttribute(writer);
}

static void writeDoubleAttribute(AttributeWriter* writer, const char* key, double value) {
    beginAttribute(writer, key);

    // proto3 JSON encodes non-finite doubles as quoted strings; a bare nan/inf
    // from %g would make the whole batch document invalid JSON.
    if (isnan(value)) {
        appendStringInfoString(writer->builder, "\"doubleValue\":\"NaN\"");
    }
    else if (isinf(value)) {
        appendStringInfoString(writer->builder, value > 0 ? "\"doubleValue\":\"Infinity\"" : "\"doubleValue\":\"-Infinity\"");
    }
    else {
        appendStringInfo(writer->builder, "\"doubleValue\":%.17g", value);
    }

    endAttribute(writer);
}

static void writeBoolAttribute(AttributeWriter* writer, const char* key, bool value) {
    beginAttribute(writer, key);
    appendStringInfo(writer->builder, "\"boolValue\":%s", value ? "true" : "false");
    endAttribute(writer);
}

static void writeBufferUsageAttributes(AttributeWriter* writer, const char* prefix, const BufferUsage* usage) {
    char key[96];

    snprintf(key, sizeof(key), "%s.shared_blks_hit", prefix);
    writeIntAttribute(writer, key, usage->shared_blks_hit);
    snprintf(key, sizeof(key), "%s.shared_blks_read", prefix);
    writeIntAttribute(writer, key, usage->shared_blks_read);
    snprintf(key, sizeof(key), "%s.shared_blks_dirtied", prefix);
    writeIntAttribute(writer, key, usage->shared_blks_dirtied);
    snprintf(key, sizeof(key), "%s.shared_blks_written", prefix);
    writeIntAttribute(writer, key, usage->shared_blks_written);
    snprintf(key, sizeof(key), "%s.local_blks_hit", prefix);
    writeIntAttribute(writer, key, usage->local_blks_hit);
    snprintf(key, sizeof(key), "%s.local_blks_read", prefix);
    writeIntAttribute(writer, key, usage->local_blks_read);
    snprintf(key, sizeof(key), "%s.local_blks_dirtied", prefix);
    writeIntAttribute(writer, key, usage->local_blks_dirtied);
    snprintf(key, sizeof(key), "%s.local_blks_written", prefix);
    writeIntAttribute(writer, key, usage->local_blks_written);
    snprintf(key, sizeof(key), "%s.temp_blks_read", prefix);
    writeIntAttribute(writer, key, usage->temp_blks_read);
    snprintf(key, sizeof(key), "%s.temp_blks_written", prefix);
    writeIntAttribute(writer, key, usage->temp_blks_written);
    snprintf(key, sizeof(key), "%s.shared_blk_read_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(usage->shared_blk_read_time));
    snprintf(key, sizeof(key), "%s.shared_blk_write_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(usage->shared_blk_write_time));
    snprintf(key, sizeof(key), "%s.local_blk_read_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(usage->local_blk_read_time));
    snprintf(key, sizeof(key), "%s.local_blk_write_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(usage->local_blk_write_time));
    snprintf(key, sizeof(key), "%s.temp_blk_read_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(usage->temp_blk_read_time));
    snprintf(key, sizeof(key), "%s.temp_blk_write_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(usage->temp_blk_write_time));
}

static void writeWalUsageAttributes(AttributeWriter* writer, const char* prefix, const WalUsage* usage) {
    char key[96];

    snprintf(key, sizeof(key), "%s.records", prefix);
    writeIntAttribute(writer, key, usage->wal_records);
    snprintf(key, sizeof(key), "%s.fpi", prefix);
    writeIntAttribute(writer, key, usage->wal_fpi);
    snprintf(key, sizeof(key), "%s.bytes", prefix);
    writeIntAttribute(writer, key, usage->wal_bytes);
    snprintf(key, sizeof(key), "%s.buffers_full", prefix);
    writeIntAttribute(writer, key, usage->wal_buffers_full);
}

static void writeJitAttributes(AttributeWriter* writer, const char* prefix, const JitInstrumentation* jit) {
    char key[96];

    snprintf(key, sizeof(key), "%s.created_functions", prefix);
    writeIntAttribute(writer, key, jit->created_functions);
    snprintf(key, sizeof(key), "%s.generation_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(jit->generation_counter));
    snprintf(key, sizeof(key), "%s.deform_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(jit->deform_counter));
    snprintf(key, sizeof(key), "%s.inlining_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(jit->inlining_counter));
    snprintf(key, sizeof(key), "%s.optimization_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(jit->optimization_counter));
    snprintf(key, sizeof(key), "%s.emission_seconds", prefix);
    writeDoubleAttribute(writer, key, INSTR_TIME_GET_DOUBLE(jit->emission_counter));
}

static void writeQueryAttributes(AttributeWriter* writer, const QuerySpanData* data) {
    writeOptionalStringAttribute(writer, "db.postgresql.statement.command", data->commandTag);

    if (data->hasResourceUsage) {
        writeBufferUsageAttributes(writer, "db.postgresql.buffers", &data->bufferUsage);
        writeWalUsageAttributes(writer, "db.postgresql.wal", &data->walUsage);
    }

    if (data->hasJitFlags) {
        writeIntAttribute(writer, "db.postgresql.jit.flags", data->jitFlags);
    }

    if (data->hasJit) {
        writeJitAttributes(writer, "db.postgresql.jit", &data->jit);
    }

    if (data->hasWorkerJit) {
        writeJitAttributes(writer, "db.postgresql.jit.worker", &data->workerJit);
    }
}

static void writePlannerAttributes(AttributeWriter* writer, const PlannerSpanData* data) {
    writeStringAttribute(writer, "db.postgresql.planner.command", data->commandTag);
    writeIntAttribute(writer, "db.postgresql.planner.cursor_options", data->cursorOptions);

    if (!data->hasPlanStats) {
        return;
    }

    writeStringAttribute(writer, "db.postgresql.plan.top_node_type", data->topNodeType);
    writeDoubleAttribute(writer, "db.postgresql.plan.startup_cost", data->startupCost);
    writeDoubleAttribute(writer, "db.postgresql.plan.total_cost", data->totalCost);
    writeDoubleAttribute(writer, "db.postgresql.plan.estimated_rows", data->estimatedRows);
    writeIntAttribute(writer, "db.postgresql.plan.estimated_width", data->estimatedWidth);
    writeIntAttribute(writer, "db.postgresql.plan.node_count", data->nodeCount);
    writeIntAttribute(writer, "db.postgresql.plan.max_depth", data->maxDepth);
    writeIntAttribute(writer, "db.postgresql.plan.scan_count", data->scanCount);
    writeIntAttribute(writer, "db.postgresql.plan.join_count", data->joinCount);
    writeBoolAttribute(writer, "db.postgresql.plan.parallel_mode_needed", data->parallelModeNeeded);
    writeBoolAttribute(writer, "db.postgresql.plan.has_parallel_nodes", data->parallelNodeCount > 0);
    writeIntAttribute(writer, "db.postgresql.plan.parallel_node_count", data->parallelNodeCount);
    writeIntAttribute(writer, "db.postgresql.plan.workers_planned", data->workersPlanned);
    writeIntAttribute(writer, "db.postgresql.plan.relation_rte_count", data->relationRteCount);
    writeIntAttribute(writer, "db.postgresql.plan.subquery_rte_count", data->subqueryRteCount);
    writeIntAttribute(writer, "db.postgresql.plan.function_rte_count", data->functionRteCount);
    writeIntAttribute(writer, "db.postgresql.plan.jit_flags", data->jitFlags);
}

static void writeUtilityAttributes(AttributeWriter* writer, const UtilitySpanData* data) {
    writeStringAttribute(writer, "db.postgresql.utility.command", data->commandTag);
    writeStringAttribute(writer, "db.postgresql.utility.context", data->context);
}

static void writePlanNodeAttributes(AttributeWriter* writer, const PlanNodeSpanData* data) {
    writeStringAttribute(writer, "db.postgresql.plan.node_type", data->nodeType);
    writeIntAttribute(writer, "db.postgresql.plan.node_id", data->nodeId);
    writeDoubleAttribute(writer, "db.postgresql.plan.rows", data->rows);
    writeDoubleAttribute(writer, "db.postgresql.plan.loops", data->loops);
    writeDoubleAttribute(writer, "db.postgresql.plan.total_seconds", data->totalSeconds);
    writeDoubleAttribute(writer, "db.postgresql.plan.startup_seconds", data->startupSeconds);
    writeDoubleAttribute(writer, "db.postgresql.plan.estimated_rows", data->estimatedRows);
    writeDoubleAttribute(writer, "db.postgresql.plan.startup_cost", data->startupCost);
    writeDoubleAttribute(writer, "db.postgresql.plan.total_cost", data->totalCost);

    if (data->loops > 0.0) {
        writeDoubleAttribute(writer, "db.postgresql.plan.actual_rows_per_loop", data->rows / data->loops);
    }

    writeBoolAttribute(writer, "db.postgresql.plan.parallel_aware", data->parallelAware);
    writeBoolAttribute(writer, "db.postgresql.plan.parallel_safe", data->parallelSafe);

    writeOptionalStringAttribute(writer, "db.postgresql.plan.relation_name", data->relationName);
    writeOptionalStringAttribute(writer, "db.postgresql.plan.index_name", data->indexName);
    writeOptionalStringAttribute(writer, "db.postgresql.plan.join_type", data->joinType);
    writeOptionalStringAttribute(writer, "db.postgresql.plan.modify_operation", data->modifyOperation);

    writeBufferUsageAttributes(writer, "db.postgresql.plan.buffers", &data->bufferUsage);
    writeWalUsageAttributes(writer, "db.postgresql.plan.wal", &data->walUsage);

    if (data->hasGatherInfo) {
        writeIntAttribute(writer, "db.postgresql.parallel.workers_planned", data->workersPlanned);
        writeIntAttribute(writer, "db.postgresql.parallel.workers_launched", data->workersLaunched);
        if (data->hasSingleCopy) {
            writeBoolAttribute(writer, "db.postgresql.parallel.single_copy", data->singleCopy);
        }
    }

    if (data->hasWorkerSummary) {
        writeIntAttribute(writer, "db.postgresql.parallel.worker_count", data->workerCount);
        writeIntAttribute(writer, "db.postgresql.parallel.active_worker_count", data->activeWorkerCount);
        writeDoubleAttribute(writer, "db.postgresql.parallel.worker_rows", data->workerRows);
        writeDoubleAttribute(writer, "db.postgresql.parallel.worker_loops", data->workerLoops);
        writeDoubleAttribute(writer, "db.postgresql.parallel.worker_total_seconds", data->workerTotalSeconds);
        writeDoubleAttribute(writer, "db.postgresql.parallel.worker_max_seconds", data->workerMaxSeconds);
    }
}

static void writeWorkerAttributes(AttributeWriter* writer, const WorkerSpanData* data) {
    writeStringAttribute(writer, "db.postgresql.plan.node_type", data->nodeType);
    writeIntAttribute(writer, "db.postgresql.plan.node_id", data->nodeId);
    writeIntAttribute(writer, "db.postgresql.parallel.worker_index", data->workerIndex);
    writeDoubleAttribute(writer, "db.postgresql.parallel.worker_rows", data->rows);
    writeDoubleAttribute(writer, "db.postgresql.parallel.worker_loops", data->loops);
    writeDoubleAttribute(writer, "db.postgresql.parallel.worker_total_seconds", data->totalSeconds);
    writeDoubleAttribute(writer, "db.postgresql.parallel.worker_startup_seconds", data->startupSeconds);
    writeBufferUsageAttributes(writer, "db.postgresql.parallel.worker.buffers", &data->bufferUsage);
    writeWalUsageAttributes(writer, "db.postgresql.parallel.worker.wal", &data->walUsage);
}

static void serializeAttributes(StringInfo builder, const Span* span) {
    AttributeWriter writer = {
        .builder = builder,
        .first = true,
    };

    appendStringInfoChar(builder, '[');

    writeStringAttribute(&writer, "db.system", "postgresql");

    switch (span->type) {
    case PG_EATRACE_SPAN_QUERY:
        writeQueryAttributes(&writer, &span->data.query);
        break;
    case PG_EATRACE_SPAN_PLANNING:
        writePlannerAttributes(&writer, &span->data.planner);
        break;
    case PG_EATRACE_SPAN_UTILITY:
        writeUtilityAttributes(&writer, &span->data.utility);
        break;
    case PG_EATRACE_SPAN_PLAN_NODE:
        writePlanNodeAttributes(&writer, &span->data.planNode);
        break;
    case PG_EATRACE_SPAN_WORKER:
        writeWorkerAttributes(&writer, &span->data.worker);
        break;
    }

    // Plan-node and worker timings are reconstructed from accumulated durations,
    // not absolute timestamps; keep them marked as approximations.
    if (span->type == PG_EATRACE_SPAN_PLAN_NODE || span->type == PG_EATRACE_SPAN_WORKER) {
        writeBoolAttribute(&writer, "pg_eatrace.timing_approximation", true);
    }

    if (span->statusCode == PG_EATRACE_SPAN_STATUS_ERROR) {
        writeOptionalStringAttribute(&writer, "db.postgresql.error.sqlstate", span->sqlState);
        writeOptionalStringAttribute(&writer, "db.postgresql.error.message", span->statusMessage);
    }

    appendStringInfoChar(builder, ']');
}

// OTLP span kind: query and utility spans are the server side of a client
// call; everything else is internal database work.
static int spanKind(const Span* span) {
    switch (span->type) {
    case PG_EATRACE_SPAN_QUERY:
    case PG_EATRACE_SPAN_UTILITY:
        return 2;
    default:
        return 1;
    }
}

static void serializeSpan(StringInfo builder, const Span* span) {
    appendStringInfoChar(builder, '{');

    appendStringInfoString(builder, "\"traceId\":\"");
    appendIdHex(builder, span->traceId, PG_EATRACE_TRACE_ID_SIZE);
    appendStringInfoChar(builder, '"');

    appendStringInfoString(builder, ",\"spanId\":\"");
    appendIdHex(builder, span->spanId, PG_EATRACE_SPAN_ID_SIZE);
    appendStringInfoChar(builder, '"');

    appendStringInfoString(builder, ",\"parentSpanId\":\"");
    appendIdHex(builder, span->parentSpanId, PG_EATRACE_SPAN_ID_SIZE);
    appendStringInfoChar(builder, '"');

    appendStringInfoString(builder, ",\"name\":\"");
    appendToJsonString(builder, span->name);
    appendStringInfoChar(builder, '"');

    appendStringInfo(builder, ",\"kind\":%d", spanKind(span));

    // OTLP timestamps are uint64 nanoseconds encoded as decimal strings
    appendStringInfo(builder, ",\"startTimeUnixNano\":\"" UINT64_FORMAT "\"", span->startTimeUnixNano);
    appendStringInfo(builder, ",\"endTimeUnixNano\":\"" UINT64_FORMAT "\"", span->endTimeUnixNano);

    appendStringInfoString(builder, ",\"attributes\":");
    serializeAttributes(builder, span);

    if (span->statusCode != PG_EATRACE_SPAN_STATUS_UNSET) {
        appendStringInfo(builder, ",\"status\":{\"code\":%d", span->statusCode);
        if (span->statusMessage[0] != '\0') {
            appendStringInfoString(builder, ",\"message\":\"");
            appendToJsonString(builder, span->statusMessage);
            appendStringInfoChar(builder, '"');
        }
        appendStringInfoChar(builder, '}');
    }

    appendStringInfoChar(builder, '}');
}

void otlpBatchBegin(OtlpBatch* batch) {
    batch->spanCount = 0;
    initStringInfo(&batch->builder);

    appendStringInfoString(&batch->builder, "{\"resourceSpans\":[{");
    appendStringInfoString(&batch->builder, "\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":\"postgresql\"}}]}");
    appendStringInfoString(&batch->builder, ",\"scopeSpans\":[{");
    appendStringInfoString(&batch->builder, "\"scope\":{\"name\":\"pg_eatrace\",\"version\":\"0.0\"}");
    appendStringInfoString(&batch->builder, ",\"spans\":[");
}

void otlpBatchAddSpan(OtlpBatch* batch, Span* span) {
    if (batch->spanCount > 0) {
        appendStringInfoChar(&batch->builder, ',');
    }

    serializeSpan(&batch->builder, span);
    batch->spanCount++;
}

char* otlpBatchFinish(OtlpBatch* batch) {
    appendStringInfoString(&batch->builder, "]}]}]}");
    return batch->builder.data;
}

char* serializeSpanAsOtlpJson(Span* span) {
    OtlpBatch batch;
    otlpBatchBegin(&batch);
    otlpBatchAddSpan(&batch, span);
    return otlpBatchFinish(&batch);
}
