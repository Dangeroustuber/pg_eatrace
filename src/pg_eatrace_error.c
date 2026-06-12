#include "postgres.h"

#include "utils/elog.h"
#include "utils/memutils.h"

#include "pg_eatrace_error.h"

static void unpackSqlStateCode(int sqlerrcode, char sqlState[6]) {
    sqlState[0] = PGUNSIXBIT(sqlerrcode);
    sqlState[1] = PGUNSIXBIT(sqlerrcode >> 6);
    sqlState[2] = PGUNSIXBIT(sqlerrcode >> 12);
    sqlState[3] = PGUNSIXBIT(sqlerrcode >> 18);
    sqlState[4] = PGUNSIXBIT(sqlerrcode >> 24);
    sqlState[5] = '\0';
}

void copyCaughtError(CaughtError* error) {
    ErrorData* errorData;
    MemoryContext oldContext;

    error->sqlState[0] = '\0';
    error->message[0] = '\0';

    oldContext = MemoryContextSwitchTo(TopMemoryContext);
    errorData = CopyErrorData();
    MemoryContextSwitchTo(oldContext);

    if (!errorData) {
        return;
    }

    unpackSqlStateCode(errorData->sqlerrcode, error->sqlState);
    strlcpy(error->message, errorData->message ? errorData->message : "", sizeof(error->message));

    FreeErrorData(errorData);
}
