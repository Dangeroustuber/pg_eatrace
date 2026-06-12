#include "postgres.h"

#include "pg_eatrace_otlp_json.h"
#include "pg_eatrace_span.h"
#include "pg_eatrace_trace_context.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static const uint8 testTraceId[PG_EATRACE_TRACE_ID_SIZE] = {
    0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
    0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89
};
static const uint8 testSpanId[PG_EATRACE_SPAN_ID_SIZE] = {
    0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89
};
static const uint8 testParentSpanId[PG_EATRACE_SPAN_ID_SIZE] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
};

static void expectContains(const char* json, const char* needle, const char* description) {
    if (strstr(json, needle) == NULL) {
        fprintf(stderr, "not ok - %s\nmissing: %s\njson: %s\n", description, needle, json);
        failures++;
    }
}

static void expectNotContains(const char* json, const char* needle, const char* description) {
    if (strstr(json, needle) != NULL) {
        fprintf(stderr, "not ok - %s\nunexpected: %s\njson: %s\n", description, needle, json);
        failures++;
    }
}

static void expectTrue(bool result, const char* description) {
    if (!result) {
        fprintf(stderr, "not ok - %s\n", description);
        failures++;
    }
}

static void expectFalse(bool result, const char* description) {
    if (result) {
        fprintf(stderr, "not ok - %s\n", description);
        failures++;
    }
}

static void expectIdEquals(const uint8* bytes, int length, const char* expectedHex, const char* description) {
    char actualHex[PG_EATRACE_TRACE_ID_HEX_SIZE];

    formatIdBytes(actualHex, bytes, length);
    if (strcmp(actualHex, expectedHex) != 0) {
        fprintf(stderr, "not ok - %s\nexpected: %s\nactual: %s\n", description, expectedHex, actualHex);
        failures++;
    }
}

static void fillQuerySpan(Span* span) {
    initSpan(span, PG_EATRACE_SPAN_QUERY, testTraceId, testParentSpanId, testSpanId, "postgres.query",
        UINT64CONST(1700000000000000000), UINT64CONST(1700000001000000000));

    span->data.query.hasResourceUsage = true;
    span->data.query.bufferUsage.shared_blks_hit = 42;
    span->data.query.hasJitFlags = true;
    span->data.query.jitFlags = 7;
}

int main(void) {
    Span span;
    char* json;
    uint8 traceId[PG_EATRACE_TRACE_ID_SIZE];
    uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE];
    bool sampled;

    fillQuerySpan(&span);
    json = serializeSpanAsOtlpJson(&span);

    expectContains(json, "\"resourceSpans\":[", "uses OTLP resourceSpans envelope");
    expectContains(json, "\"scopeSpans\":[", "uses OTLP scopeSpans envelope");
    expectContains(json, "\"key\":\"service.name\",\"value\":{\"stringValue\":\"postgresql\"}", "sets stable service.name resource attribute");
    expectContains(json, "\"scope\":{\"name\":\"pg_eatrace\",\"version\":\"0.0\"}", "sets stable instrumentation scope");
    expectContains(json, "\"traceId\":\"abcdef0123456789abcdef0123456789\"", "serializes lowercase 32-char traceId");
    expectContains(json, "\"spanId\":\"abcdef0123456789\"", "serializes lowercase 16-char spanId");
    expectContains(json, "\"parentSpanId\":\"0123456789abcdef\"", "serializes lowercase 16-char parentSpanId");
    expectContains(json, "\"kind\":2", "query span uses SPAN_KIND_SERVER");
    expectContains(json, "\"key\":\"db.system\",\"value\":{\"stringValue\":\"postgresql\"}", "emits db.system on every span");
    expectContains(json, "\"key\":\"db.postgresql.buffers.shared_blks_hit\",\"value\":{\"intValue\":\"42\"}", "expands buffer usage into intValue attributes");
    expectContains(json, "\"key\":\"db.postgresql.jit.flags\",\"value\":{\"intValue\":\"7\"}", "serializes jit flags when present");
    expectContains(json, "\"startTimeUnixNano\":\"1700000000000000000\"", "serializes nanosecond start timestamp as decimal string");
    expectNotContains(json, "\"status\"", "omits status when unset");
    pfree(json);

    fillQuerySpan(&span);
    addErrorToSpan(&span, "22012", "division by zero");
    json = serializeSpanAsOtlpJson(&span);
    expectContains(json, "\"status\":{\"code\":2,\"message\":\"division by zero\"}", "serializes OTLP error status");
    expectContains(json, "\"key\":\"db.postgresql.error.sqlstate\",\"value\":{\"stringValue\":\"22012\"}", "serializes SQLSTATE attribute");
    expectContains(json, "\"key\":\"db.postgresql.error.message\",\"value\":{\"stringValue\":\"division by zero\"}", "serializes error message attribute");
    pfree(json);

    fillQuerySpan(&span);
    addErrorToSpan(&span, "XX000", "fail \"quoted\" back\\slash\n\t\001");
    json = serializeSpanAsOtlpJson(&span);
    expectContains(json, "fail \\\"quoted\\\"", "escapes quotes in strings");
    expectContains(json, "back\\\\slash", "escapes backslashes in strings");
    expectContains(json, "\\n", "escapes newlines in strings");
    expectContains(json, "\\t", "escapes tabs in strings");
    expectContains(json, "\\u0001", "escapes control characters in strings");
    pfree(json);

    initSpan(&span, PG_EATRACE_SPAN_PLAN_NODE, testTraceId, testParentSpanId, NULL, "Seq Scan",
        UINT64CONST(1700000000000000000), UINT64CONST(1700000000500000000));
    strlcpy(span.data.planNode.nodeType, "Seq Scan", sizeof(span.data.planNode.nodeType));
    strlcpy(span.data.planNode.relationName, "orders", sizeof(span.data.planNode.relationName));
    span.data.planNode.rows = 10.0;
    span.data.planNode.loops = 4.0;
    span.data.planNode.totalSeconds = NAN;
    json = serializeSpanAsOtlpJson(&span);
    expectContains(json, "\"kind\":1", "plan-node span uses SPAN_KIND_INTERNAL");
    expectContains(json, "\"key\":\"db.postgresql.plan.node_type\",\"value\":{\"stringValue\":\"Seq Scan\"}", "serializes plan node type");
    expectContains(json, "\"key\":\"db.postgresql.plan.relation_name\",\"value\":{\"stringValue\":\"orders\"}", "serializes optional relation name");
    expectNotContains(json, "db.postgresql.plan.index_name", "omits empty optional strings");
    expectContains(json, "\"key\":\"db.postgresql.plan.actual_rows_per_loop\",\"value\":{\"doubleValue\":2.5}", "derives rows per loop");
    expectContains(json, "\"doubleValue\":\"NaN\"", "encodes non-finite doubles as proto3 JSON strings");
    expectContains(json, "\"key\":\"pg_eatrace.timing_approximation\",\"value\":{\"boolValue\":true}", "marks plan-node timing as approximate");
    pfree(json);

    expectTrue(parseTraceParentValueWithSampling("00-ABCDEF0123456789ABCDEF0123456789-0123456789ABCDEF-01", traceId, parentSpanId, &sampled), "parses valid traceparent with sampled flag");
    expectTrue(sampled, "detects W3C sampled flag when low bit is set");
    expectIdEquals(traceId, PG_EATRACE_TRACE_ID_SIZE, "abcdef0123456789abcdef0123456789", "parses trace ID bytes from uppercase hex");
    expectIdEquals(parentSpanId, PG_EATRACE_SPAN_ID_SIZE, "0123456789abcdef", "parses parent span ID bytes");

    expectTrue(parseTraceParentValueWithSampling("00-abcdef0123456789abcdef0123456789-0123456789abcdef-00", traceId, parentSpanId, &sampled), "parses valid traceparent without sampled flag");
    expectFalse(sampled, "detects unsampled W3C trace flags");

    expectTrue(parseTraceParentValueWithSampling("00-abcdef0123456789abcdef0123456789-0123456789abcdef-03", traceId, parentSpanId, &sampled), "parses traceparent with additional trace flags");
    expectTrue(sampled, "uses low trace-flags bit for sampled decision");

    expectFalse(parseTraceParentValueWithSampling("00-abcdef0123456789abcdef0123456789-0123456789abcdef-0x", traceId, parentSpanId, &sampled), "rejects invalid trace flags");
    expectFalse(sampled, "leaves sampled false for invalid traceparent");

    expectFalse(parseTraceParentValueWithSampling("ff-abcdef0123456789abcdef0123456789-0123456789abcdef-01", traceId, parentSpanId, &sampled), "rejects the invalid ff version");
    expectFalse(parseTraceParentValueWithSampling("00-00000000000000000000000000000000-0123456789abcdef-01", traceId, parentSpanId, &sampled), "rejects all-zero trace ID");
    expectFalse(parseTraceParentValueWithSampling("00-abcdef0123456789abcdef0123456789-0000000000000000-01", traceId, parentSpanId, &sampled), "rejects all-zero parent span ID");

    expectTrue(parseTraceParentFromQueryWithSampling("/*traceparent='00-abcdef0123456789abcdef0123456789-0123456789abcdef-01'*/ select 1", traceId, parentSpanId, &sampled), "parses traceparent from a SQL comment");
    expectIdEquals(traceId, PG_EATRACE_TRACE_ID_SIZE, "abcdef0123456789abcdef0123456789", "parses trace id from comment");
    expectIdEquals(parentSpanId, PG_EATRACE_SPAN_ID_SIZE, "0123456789abcdef", "parses parent span id from comment");
    expectTrue(sampled, "detects sampled flag from comment traceparent");

    expectFalse(parseTraceParentFromQueryWithSampling("select 'traceparent='00-abcdef0123456789abcdef0123456789-0123456789abcdef-01'", traceId, parentSpanId, &sampled), "ignores traceparent outside a comment");
    expectFalse(parseTraceParentFromQueryWithSampling("select 1 /* traceparent= no value here */", traceId, parentSpanId, &sampled), "ignores comment without a valid traceparent value");

    if (failures > 0) {
        return 1;
    }

    printf("ok - otlp json serialization\n");
    return 0;
}
