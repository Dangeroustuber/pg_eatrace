#include "postgres.h"

#include "pg_eatrace_utility_span.h"

static const char* utilityContextName(ProcessUtilityContext context) {
    switch (context) {
    case PROCESS_UTILITY_TOPLEVEL:
        return "toplevel";
    case PROCESS_UTILITY_QUERY:
        return "query";
    case PROCESS_UTILITY_QUERY_NONATOMIC:
        return "query_nonatomic";
    case PROCESS_UTILITY_SUBCOMMAND:
        return "subcommand";
    default:
        return "unknown";
    }
}

void buildUtilitySpan(Span* span, ProcessUtilityContext context, uint64 startNs, uint64 endNs, const char* commandTagName, const uint8 traceId[PG_EATRACE_TRACE_ID_SIZE], const uint8 parentSpanId[PG_EATRACE_SPAN_ID_SIZE], const uint8 utilitySpanId[PG_EATRACE_SPAN_ID_SIZE]) {
    char spanName[64];

    snprintf(spanName, sizeof(spanName), "postgres.utility %s", commandTagName);

    initSpan(span, PG_EATRACE_SPAN_UTILITY, traceId, parentSpanId, utilitySpanId, spanName, startNs, endNs);

    strlcpy(span->data.utility.commandTag, commandTagName, sizeof(span->data.utility.commandTag));
    strlcpy(span->data.utility.context, utilityContextName(context), sizeof(span->data.utility.context));
}
