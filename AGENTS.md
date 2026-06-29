# AGENTS.md

## Project

`pg_eatrace` is a PostgreSQL extension that makes Postgres a participant in distributed tracing by emitting OpenTelemetry spans for database-internal work. Inspired by `DataDog/pg_tracing`, but an independent minimal implementation built for learning PostgreSQL internals.

## Architecture Invariants

1. Trace context comes in through SQLCommenter-style W3C `traceparent` SQL comments:
   `/*traceparent='00-<trace_id>-<parent_span_id>-01'*/ SELECT ...`
2. Query rows return normally; spans are never returned over the database connection.
3. Spans export out-of-band: backends enqueue `Span` structs into a fixed-size shared-memory ring buffer (named LWLock tranche); a static background worker drains the queue and exports OTLP/JSON over HTTP to `pg_eatrace.collector_url`, using the built-in minimal HTTP/1.1 client in `pg_eatrace_http.c` (plain `http://` only, `Connection: close`, no redirects/proxies/TLS; the extension's only shared-library dependency is libc). With an empty URL the worker logs drained spans instead.

Never reintroduce synchronous backend HTTP export or per-query span pushing. Learning shortcuts (logging spans, prototype sync POST) only when explicitly requested.

Export is at-most-once and lossy by design: a full ring drops new spans (`droppedCount`); a failed export batch is not retried. The worker flushes backlog in bounded batches (`max_export_batch`) and stops draining on the first failure. Retries/backoff are future work.

## Span Families (all implemented)

- `postgres.query`: `planner_hook` generates the query span id before planning; `ExecutorEnd_hook` emits the span on success, so it covers planning + execution. Executor failures emit errored query spans from the start/run/finish catch paths.
- `postgres.planning`: from `planner_hook`, child of the query span; planner failure emits errored query + planner spans.
- Utility spans: from `ProcessUtility_hook` with command tag and utility context; errored variant on failure.
- Plan-node child spans: `ExecutorEnd_hook` walks the executed `PlanState` tree on success. Timing is approximate (PostgreSQL exposes accumulated durations, not absolute node timestamps) and marked `pg_eatrace.timing_approximation=true`.
- Optional per-worker child spans (`pg_eatrace.parallel_worker_spans`), built from leader-side `worker_instrument`.

## Behavioral Contracts

Hold these when changing code; they double as the review checklist.

- All hooks use the save-and-chain pattern; disabled tracing must still chain.
- `pg_eatrace.enabled` is the master kill switch and beats `span_generation` and sampling.
- `span_generation`: `none` = no spans; `top-level` = skip nested statements under an active pg_eatrace span; `all` = nested statements without their own traceparent inherit the active span as parent.
- Invalid traceparents create no spans. The W3C sampled flag is required; `sample_rate` only downsamples caller-sampled traces and never overrides an upstream unsampled decision. The decision is deterministic per trace (OTel TraceIdRatioBased: low 8 bytes of the trace id), so all statements of one trace agree.
- Parallel workers never generate spans (`IsParallelWorker()` guard in `spanGenerationEnabled`); only the leader emits.
- Instrumentation is armed only for traced statements: OR `INSTRUMENT_TIMER|BUFFERS|WAL` into `queryDesc->instrument_options` (never assign); allocate `queryDesc->totaltime` only after `standard_ExecutorStart`, in `es_query_cxt`; call `InstrEndLoop` before reading accumulated totals (totaltime and plan-node instruments).
- Trace state lives in backend-local hashes keyed by `QueryDesc*`/`PlannedStmt*` plus an active-span stack. Executor hooks tear down per-query state; a `RegisterXactCallback` clears all state and resets the stack at commit/abort/prepare, so paths that bypass `ExecutorEnd` (portals dropped at abort, plans never executed) cannot leave dangling-pointer keys or leaked stack frames.
- Error paths: `PG_TRY`/`PG_CATCH`, copy `ErrorData`, emit error span (OTLP error status, SQLSTATE, short message), `PG_RE_THROW`.
- Trace/span IDs are raw bytes internally (16/8); hex exists only at the traceparent-parse and OTLP-serialize boundaries, emitted lowercase. OTLP JSON keeps `resourceSpans -> scopeSpans -> spans` with nanosecond timestamps as decimal strings.
- `Span` payloads are typed per span family (a tagged union of counter structs, ~0.9 KB total); the serializer expands them into OTLP attributes, so attribute keys exist only as literals in `pg_eatrace_otlp_json.c`. Adding span data means extending the payload struct and its serializer case, never a generic attribute slot.
- Spans never carry SQL text or query ids. Statements are identified by command tags, span names, and plan-node detail (relation/index names); instrumented clients carry `db.query.text` on their own span per OTel semconv.
- JIT/worker pointers (`estate`, `es_jit`, `es_jit_worker_instr`, `worker_instrument`) are read only after NULL checks; per-worker data only when its loop count is positive.
- `pg_eatrace_status()` exposes narrow queue/export state only (never span payloads) and errors if shared memory is absent. The OUT parameters in `pg_eatrace--0.0.sql` are the single source of truth for the row shape (`get_call_result_type`); changing them requires only matching the value order in C.
- The worker waits on its own `MyLatch` (timeout-driven drain); there is no shared latch or latch-ownership state. It uses the standard `SignalHandlerForShutdownRequest`/`SignalHandlerForConfigReload` handlers, and the HTTP client's socket waits go through `WaitLatchOrSocket` with a monotonic deadline, so shutdown requests and postmaster death interrupt an in-flight export instead of waiting out its timeout.
- User queries never block on a full queue.

## Known Limitations

- Cached/prepared plans: only the first execution gets a planning+execution query span; later executions are execution-scoped. The planner span is enqueued with its query span (deferred from `planner_hook` to `emitSuccessfulQuerySpan`/`emitFailedQuerySpan` via `PlannerTraceState.plannerSpan`), so a cached/nested statement whose query span is never emitted drops its planner span instead of orphaning it.
- Failed executor queries emit the query-level error span but no partial plan-node spans.
- Error spans embed the server error message, which can contain data values.
- Export transport is plain HTTP/1.1 without TLS; `https://` collector URLs are rejected. The intended deployment is an in-cluster collector agent (e.g. Grafana Alloy) reached over the pod network. If TLS is ever needed, link OpenSSL (present wherever PostgreSQL runs) rather than reintroducing libcurl.

## GUCs

- `pg_eatrace.enabled` (bool, on): master backend switch.
- `pg_eatrace.span_generation` (`top-level` | `all` | `none`, default `top-level`).
- `pg_eatrace.sample_rate` (0.0–1.0, default 1.0).
- `pg_eatrace.collector_url` (worker export target; SIGHUP; plain `http://` only — a GUC check hook rejects anything else, with empty meaning log-instead-of-export).
- `pg_eatrace.max_spans` (queue capacity, `PGC_POSTMASTER`, default 5000, max 10000; a `Span` is ~0.9 KB, so capacity sizes shared memory directly).
- `pg_eatrace.export_interval_ms` (default 1000; SIGHUP).
- `pg_eatrace.max_export_batch` (default 500, max 10000; SIGHUP).
- `pg_eatrace.parallel_worker_spans` (bool, off).

The worker re-reads the SIGHUP GUCs on `pg_reload_conf()`. Do not add logs/metrics endpoint configuration until the extension actually emits logs or metrics.

## Coding Style

- Designated initializers for structs initialized at declaration (C99 nested `.a.b.c` preferred). Structs initialized through output-parameter pointers stay as-is.
- Do not manually reformat code during edits; `pg_indent` runs as a separate final pass.

## Collaboration Rules

The user is learning PostgreSQL internals. Default to explanation and discussion, not code generation.

- Do not implement features unless explicitly asked; keep requested changes minimal and focused; no unrelated refactoring.
- Prefer small illustrative snippets; push back on incorrect assumptions; cite PostgreSQL source files, structs, and functions (`executor/execdesc.h`, `executor/executor.h`, `executor/instrument.h`, `contrib/auto_explain/auto_explain.c`).
- In reviews, prefer finding concrete bugs over broad redesigns.
- All MVP rungs through parallel-worker spans are done; do not pull future work into current changes.
- Near-term gaps are validation and operability, not new span families: validate span topology against collector payloads, validate `span_generation = 'all'`, validate buffer/WAL/JIT attributes on representative workloads, decide the privacy posture for error messages, add extension versioning before a real `0.1`.

## Build And Environment

- PGXS; extension name `pg_eatrace`; source-built PostgreSQL 18 at `~/pg18`, cluster `~/pg18/data`, database `playground`.
- Requires `shared_preload_libraries = 'pg_eatrace'`; `make install` plus a full cluster restart after backend/worker/shmem changes.
- `make check` / `make installcheck` run the C OTLP/JSON serializer test (`test/otlp_json_test.c`); keep C-level tests as small focused binaries.
- `pg_eatrace.collector_url` may already be set in `postgresql.auto.conf`; check `SHOW pg_eatrace.collector_url`.
- Manual validation loop: `make && make install`, restart, run a query with a valid `traceparent` comment, confirm normal results plus out-of-band export (or worker log lines without a collector URL); use `pg_eatrace_status()` for queue/export visibility.
- Tests must not depend on exact timings, generated IDs, log order, or collector availability. Future SQL regression tests go through `pg_regress` and stick to stable checks (extension creates, functions exist, config loads).

## Layout

Hook orchestration lives in `src/pg_eatrace.c`; each concern has a module pair under `src/` and `include/`: planner spans, query spans, utility spans, plan-node spans, error helpers, OTLP JSON, queue, worker, HTTP client (the OTLP POST), span struct, trace state, trace context (traceparent parsing, ids, time), and SQL status.
