#ifndef PG_EATRACE_WORKER_H
#define PG_EATRACE_WORKER_H

#include "postgres.h"

extern PGDLLEXPORT void workerMain(Datum mainArg);

extern void defineWorkerGucs(void);
extern void registerWorker(void);

#endif // PG_EATRACE_WORKER_H
