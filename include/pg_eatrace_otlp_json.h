#ifndef PG_EATRACE_OTLP_JSON_H
#define PG_EATRACE_OTLP_JSON_H

#include "postgres.h"

#include "lib/stringinfo.h"

#include "pg_eatrace_span.h"

// The serializer writes OTLP/JSON directly from the internal Span struct via the
// incremental batch builder below.

// Batch builder: incrementally serializes multiple spans into one OTLP JSON request.
typedef struct OtlpBatch {
    StringInfoData builder;
    int            spanCount;
} OtlpBatch;

extern void  otlpBatchBegin(OtlpBatch* batch);
extern void  otlpBatchAddSpan(OtlpBatch* batch, Span* span);
extern char* otlpBatchFinish(OtlpBatch* batch);

extern char* serializeSpanAsOtlpJson(Span* span);

#endif // PG_EATRACE_OTLP_JSON_H
