#ifndef PG_EATRACE_STATUS_H
#define PG_EATRACE_STATUS_H

#include "postgres.h"

#include "fmgr.h"

extern PGDLLEXPORT Datum pg_eatrace_status(PG_FUNCTION_ARGS);

#endif // PG_EATRACE_STATUS_H
