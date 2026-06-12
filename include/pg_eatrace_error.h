#ifndef PG_EATRACE_ERROR_H
#define PG_EATRACE_ERROR_H

#include "postgres.h"

typedef struct CaughtError {
    char sqlState[6];
    char message[256];
} CaughtError;

// Copies the caught error's SQLSTATE and message inside a PG_CATCH block.
extern void copyCaughtError(CaughtError* error);

#endif // PG_EATRACE_ERROR_H
