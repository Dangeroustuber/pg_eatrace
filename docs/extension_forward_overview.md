# pg_eatrace Forward Work Overview

This document tracks what is finished, what remains, and what production-shape gaps are visible after the current planner-span, query-span, utility-span, plan-node-span, shared-memory queue, and background-worker implementation.

`pg_eatrace` is intentionally smaller than `DataDog/pg_tracing`, but `pg_tracing` is still a useful comparison point. Its public docs describe spans for planner, executor, utility statements, plan nodes, nested queries, triggers, parallel workers, and transaction commit, plus sampling, buffer/WAL/JIT metadata, trace context via SQLCommenter or GUC, and OTLP background export. Those are useful signals for what we may eventually want, not a requirement to clone the project feature-for-feature.

## Current Baseline

The extension now has the intended out-of-band shape:

- Trace context is accepted through SQLCommenter-style W3C `traceparent` comments.
- Trace contexts must have the W3C sampled flag set to be traced. `pg_eatrace.sample_rate` can further reduce caller-sampled traces.
- `pg_eatrace.span_generation` controls statement-level eligibility: `top-level` traces only top-level statements, `all` also traces nested statements by inheriting the active database span context, and `none` produces no spans while the extension remains loaded.
- For planned statements, `planner_hook` generates the root query span ID before calling PostgreSQL's planner.
- Planner spans are built through `planner_hook` for planned statements with trace context.
- Planner spans are named `postgres.planning`, use span kind `INTERNAL`, and are children of the eventual `postgres.query` span.
- Planner spans include SQL text, PostgreSQL command tag, cursor option attributes, and summary attributes from the finished `PlannedStmt`.
- Planner summary attributes include top plan node type, estimated top-node cost/rows/width, plan node count, plan depth, scan count, join count, relation/subquery/function RTE counts, parallel-plan indicators, planned workers, and JIT flags.
- Successful planned query spans start before planning and end after execution, so the root `postgres.query` span covers planning plus execution.
- Failed planning paths emit error planner spans with OTLP error status, SQLSTATE, and message while preserving PostgreSQL error propagation.
- Failed planning paths also emit a failed root `postgres.query` span so the planner error span has an exported parent.
- Query spans are built in executor hooks and enqueued into shared memory.
- Successful query spans are emitted from `ExecutorEnd_hook`.
- Failed execution query spans are emitted from `ExecutorStart_hook`, `ExecutorRun_hook`, or `ExecutorFinish_hook` catch paths when trace context exists.
- Error spans preserve normal PostgreSQL error behavior with `PG_TRY`, `PG_CATCH`, copied `ErrorData`, and `PG_RE_THROW`.
- Error spans are marked with OTLP status code `ERROR` and include SQLSTATE plus a short PostgreSQL error message.
- Executed plan nodes are emitted as child spans through the same queue and worker path for successful executor-backed queries.
- Plan-node spans include useful names and attributes for scans, joins, `ModifyTable`, planner estimates, actual rows, costs, and parallel flags.
- Query spans and plan-node spans include PostgreSQL buffer and WAL usage attributes when instrumentation is available.
- Query spans include JIT flags and leader/worker JIT timing attributes when PostgreSQL exposes JIT instrumentation.
- Plan-node spans summarize PostgreSQL parallel worker instrumentation when worker data is available.
- `pg_eatrace.parallel_worker_spans` can emit per-worker child spans beneath each parallelized plan-node span.
- Utility statements are traced through `ProcessUtility_hook`.
- Utility spans use PostgreSQL command tags, such as `CREATE TABLE`, `VACUUM`, or `COMMIT`, as span metadata.
- Failed utility statements also emit error spans with OTLP error status, SQLSTATE, and message while preserving PostgreSQL error propagation.
- When `pg_eatrace.span_generation = 'all'`, nested planner/executor/utility work can inherit the currently active database span as parent. This makes hidden work from functions, triggers, or SPI-style execution appear below the SQL statement that caused it.
- Backends enqueue internal `Span` structs into a fixed-size shared-memory ring buffer protected by a named LWLock tranche.
- If the ring is full, the backend drops the new span and increments `droppedCount`; user queries do not block on tracing.
- A static background worker drains the queue and exports OTLP/JSON over HTTP when `pg_eatrace.collector_url` is set.
- The worker batches all spans drained in one wakeup into one OTLP request under one `resourceSpans` / `scopeSpans` envelope.
- If `pg_eatrace.collector_url` is empty, the worker logs drained spans as a learning fallback.
- `pg_eatrace.enabled` is a runtime backend hook switch. When disabled, backends skip traceparent parsing, pg_eatrace instrumentation setup, pg_eatrace `totaltime` allocation, and span enqueueing while still chaining PostgreSQL hooks normally.
- `pg_eatrace_status()` reports queue depth, queue capacity, dropped spans, dequeued spans, export success/failure counts, last curl error, last HTTP status, last successful export timestamp, last failed export timestamp, and worker latch ownership.
- The OTLP/JSON serializer has focused C-level coverage for field shape, attribute value types, ID casing, span kinds, resource/scope fields, JSON escaping, error status, error attributes, and attribute capacity.
- The code is split so executor hooks, worker/export, queue, planner spans, plan-node spans, utility spans, error helpers, SQL status output, internal span helpers, OTLP JSON, trace state, and utility helpers have clearer module boundaries.

Important current limitation: failed executor queries emit the query-level error span, but they do not emit partial plan-node spans. That is intentional for now because executor error unwinding makes partial node instrumentation harder to interpret safely.

Current refactoring note: PostgreSQL hook orchestration intentionally remains in `src/pg_eatrace.c`. Planner span construction, utility span construction, error helpers, and `pg_eatrace_status()` now live in focused modules. Query-span helpers still live in `src/pg_eatrace.c` and are a reasonable future cleanup target if that file grows again.

## What To Add Next

The current trace now has planner, query, utility, plan-node, parallel-worker, and error spans. The next most useful work is probably trace topology and operability rather than another large span family.

Recommended next work:

1. Validate planner parent-child shape with simple selects, joins, prepared statements, and planning errors.
2. Validate nested queries, functions, triggers, and SPI behavior.
3. Add SQL text/privacy controls before treating exported payloads as production-safe.
4. Add extension versioning and an upgrade path before calling this `0.1`.
5. Consider executor phase spans only if we need visibility into `ExecutorRun` versus `ExecutorFinish`.
6. Validate buffer/WAL/JIT attributes against representative read, write, spill, and JIT queries.

### Why Nested Topology Next

Planner spans explain time before execution. Query and plan-node spans explain execution. The next gap is hidden database work started by functions, triggers, rules, or SPI.

Useful first behavior:

- Build manual tests for PL/pgSQL functions, trigger-fired SQL, and nested SPI queries.
- Decide whether nested statements should be children of the active outer database span.
- If needed, maintain a backend-local active span stack so nested spans attach to the current database span rather than always attaching to the incoming SQLCommenter parent.

This should make Grafana traces easier to read when a simple-looking SQL statement hides extra database work.

### Buffer/WAL/JIT Attributes

Status: initial implementation done.

Buffer, WAL, and JIT attributes are valuable because they explain whether execution cost came from cache hits, physical reads, temp spills, write amplification, WAL generation, or compilation. They are PostgreSQL-specific and should remain attributes on existing query and plan-node spans rather than a new span family.

Done:

- OR in `INSTRUMENT_BUFFERS` and `INSTRUMENT_WAL` alongside `INSTRUMENT_TIMER`.
- Add query-level buffer counters, buffer I/O timing counters, WAL counters, and JIT flags/timing fields.
- Add plan-node buffer counters, buffer I/O timing counters, and WAL counters.
- Add per-worker buffer and WAL counters on optional parallel-worker spans.

Remaining work:

- Validate output against read-heavy queries, write-heavy queries, temp-spill queries, and queries that actually trigger JIT.
- Decide later whether these high-cardinality PostgreSQL-specific attributes need GUC controls before production use.

## Near-Term Non-Span Priorities

### 1. `pg_eatrace.enabled`

Production and testing both need a cheap runtime kill switch once the extension is loaded through `shared_preload_libraries`.

Status: implemented.

Behavior:

- The boolean GUC is named `pg_eatrace.enabled`.
- Default is `on` while the project is learning-oriented.
- When disabled, skip traceparent parsing.
- When disabled, do not add instrumentation flags in `ExecutorStart`.
- When disabled, do not allocate `queryDesc->totaltime` for pg_eatrace.
- When disabled, do not build or enqueue query spans, utility spans, plan-node spans, or worker spans.
- Preserve hook chaining regardless of enabled state.

The worker can keep running while disabled. Disabled should only stop tracing work in backends.

Example usage:

- `SET pg_eatrace.enabled = off;` disables tracing in the current session, for users allowed to set superuser-level GUCs.
- `SET LOCAL pg_eatrace.enabled = off;` disables tracing only for the current transaction.
- `ALTER SYSTEM SET pg_eatrace.enabled = off; SELECT pg_reload_conf();` changes the cluster-level setting without unloading the extension.

### 2. Span Generation And Sampling

Status: initial implementation done.

`pg_eatrace.span_generation` controls which statement levels are eligible:

- `top-level` traces only statements that are not already running under an active pg_eatrace database span. This is the default.
- `all` traces top-level statements and nested statements. Nested statements without their own `traceparent` inherit the active database span as parent.
- `none` disables span generation while leaving the extension enabled and hooks chained.

Precedence:

- `pg_eatrace.enabled = off` wins over all other tracing settings and skips backend tracing work.
- `pg_eatrace.span_generation = 'none'` produces no spans, but is a tracing policy rather than a master operational kill switch.
- `pg_eatrace.sample_rate` applies only after a statement is eligible by `span_generation` and has a valid caller-sampled trace context or inherited active context.

Remaining work:

- Validate `all` mode with PL/pgSQL functions, triggers, and SPI-like nested execution against collector payloads.
- Decide later whether the active span stack needs counters or status visibility.

### 3. Sampling

Plan-node and worker spans can multiply quickly, so production use needs sampling.

Status: implemented.

Behavior:

- `pg_eatrace.sample_rate` ranges from `0.0` to `1.0`.
- `1.0` keeps every valid incoming `traceparent` whose W3C sampled flag is set.
- `0.0` keeps no traces.
- Incoming `traceparent` values with the sampled flag unset are not traced, regardless of local sample rate.
- Local sampling is only a downsampler; it does not override an upstream unsampled decision.

Example:

- `/*traceparent='00-<trace_id>-<parent_span_id>-01'*/ SELECT ...` is eligible for local sampling.
- `/*traceparent='00-<trace_id>-<parent_span_id>-00'*/ SELECT ...` is skipped.

Useful status additions later:

- Eligible statements.
- Sampled statements.
- Statements skipped by sampling.
- Statements skipped because tracing is disabled.

### 4. Retry And Backoff

The worker currently records export failure and drops the drained batch. That is acceptable for the MVP, but a production worker should avoid hammering a down collector.

Useful behavior:

- Add short backoff after repeated failures.
- Avoid logging every failed export while the collector is down.
- Track retry/drop counters separately from queue overflow drops.
- Track persistent export-drop reasons separately from queue overflow drops if drained batches continue to be discarded after failed export.
- Keep persistent retry queues out of scope unless explicitly needed later.

### 5. SQL Text And Parameter Privacy

`db.query.text` currently contains raw SQL text. SQL can contain literals, tenant IDs, emails, comments, or tokens.

Options before production:

- Add a GUC to disable SQL text export.
- Truncate more deliberately.
- Prefer normalized query text or `queryId` where available.
- Do not export bind parameter values by default.

`pg_tracing` has controls for parameter export and maximum parameter size; that is a useful comparison point, but `pg_eatrace` should be conservative by default.

### 6. Extension Versioning And Upgrade Path

Before calling this `0.1`, stop evolving only `pg_eatrace--0.0.sql`.

Expected work:

- Update `pg_eatrace.control` to a real version.
- Add a `pg_eatrace--0.1.sql` install script.
- Add upgrade scripts for future SQL-visible changes.
- Decide what the first stable SQL-visible surface is, especially `pg_eatrace_status()`.

## Span Families And Coverage Gaps

These are useful future ideas, not mandatory near-term work.

### Executor Phase Spans

`ExecutorRun_hook` and `ExecutorFinish_hook` now exist for error-aware spans. That makes phase spans technically easy:

- `postgres.executor.run`
- `postgres.executor.finish`

These could explain time spent producing rows versus time spent in executor finish work such as after-trigger processing. The cost is extra span volume and a trace shape that may be too noisy for ordinary queries.

Recommendation: do not add them by default yet. If added, put them behind a GUC such as `pg_eatrace.executor_phase_spans`, default `off`.

### Planner Spans

Status: initial implementation done.

Planner spans are interesting for large joins, partition-heavy schemas, complex prepared statements, and queries where planning time dominates execution time.

Done:

- Add one `postgres.planning` span through `planner_hook`.
- Preserve save-and-chain hook behavior with `previousPlanner`.
- Export planner spans through the shared-memory queue and background worker.
- Emit planner error spans with SQLSTATE and message when planning throws.
- Add command tag and cursor options as initial planner attributes.
- Add summary attributes from the produced `PlannedStmt`.
- Make planner spans children of the root query span.

Remaining work:

- Validate the query-parent/planner-child shape with joins, prepared statements, and planning-time failures.
- Add deeper planner-specific attributes, such as join search metadata, only if they are stable enough to be useful.

### Nested Queries, Functions, And Triggers

Nested executor invocations are partially supported because query state is keyed by `QueryDesc*`, and `pg_eatrace.span_generation = 'all'` now maintains a backend-local active span stack so nested statements can inherit the active database span as parent. The behavior still needs validation against real PL/pgSQL functions, trigger-fired statements, SPI, and nested query error paths.

Useful future work:

- Build manual tests for nested function calls and triggers.
- Confirm nested query spans become children under the outer active database span in `all` mode.
- Confirm nested statements are skipped in `top-level` mode.
- Confirm `none` mode and `enabled = off` both produce no spans, with `enabled = off` also skipping backend tracing setup.

This improves trace topology rather than adding an unrelated span family. The remaining work is validation and polish rather than first implementation.

### Transaction Commit Internals

`ProcessUtility_hook` can emit a utility span for `COMMIT`, but that does not break down low-level commit work.

Possible future work:

- Add transaction callback instrumentation only if commit latency becomes a useful target.
- Preserve trace context across transaction scope if commit internals need to attach to the right trace.
- Treat WAL flush, synchronous replication wait, and commit record timing carefully; these are not all visible from a simple utility hook.

### Trace Propagation

`pg_tracing` supports SQLCommenter and a `trace_context` GUC. `pg_eatrace` currently supports only SQL comments, which is acceptable for now. A future `pg_eatrace.trace_context` GUC would be a cleaner propagation path for application code that already has request trace context outside the SQL string.

Possible future work:

- Add `pg_eatrace.trace_context` as a custom string GUC that accepts the raw W3C `traceparent` value, for example `00-<trace_id>-<parent_span_id>-01`.
- Prefer `SET LOCAL pg_eatrace.trace_context = '00-...'` inside the application's existing `BEGIN` / `COMMIT` block so the context is transaction-scoped and automatically cleared.
- Treat this as PgBouncer-friendly when using transaction pooling: `BEGIN` pins a server backend for the transaction, `SET LOCAL` applies only to that transaction, and `COMMIT` / `ROLLBACK` clears it before the backend returns to the pool.
- Avoid plain session-level `SET pg_eatrace.trace_context = '00-...'` as the recommended pattern, because pooled connections can leak session state across requests.
- Keep SQLCommenter support as the current implementation until the GUC path is added.
- If both SQLCommenter and the trace-context GUC exist later, define explicit precedence before implementation.

### Parallel Workers

`pg_eatrace` records plan-level `parallel_aware` and `parallel_safe` flags, and `Gather` / `Gather Merge` nodes can appear as plan-node spans.

Status: initial implementation done.

Done:

- Aggregate `PlanState->worker_instrument` into plan-node span attributes.
- Add worker count, active worker count, worker row count, loop count, total worker seconds, and max worker seconds summaries.
- Add `Gather` / `Gather Merge` planned and launched worker counts as direct span attributes.
- Add plain `Gather` `single_copy` as a direct span attribute.
- Add `pg_eatrace.parallel_worker_spans`, default `off`, to control whether per-worker child spans are emitted.
- When enabled, emit one child span per active worker for each plan node that has worker instrumentation.
- Worker child spans include worker index, plan node ID, node type, rows, loops, total seconds, and startup seconds.

Remaining work:

- Manually validate output against real parallel plans and collector payloads.
- Per-worker span timing remains approximate. PostgreSQL exposes accumulated per-worker durations in `WorkerInstrumentation`, not exact absolute start/end timestamps for each worker.

### Buffers, WAL, JIT, And I/O

PostgreSQL instrumentation can expose buffer and WAL usage when the right instrumentation flags are enabled. `pg_tracing` exposes shared/local/temp block counters, block read/write times, WAL counters, and JIT timing fields.

Status: initial implementation done.

Done:

- OR in `INSTRUMENT_BUFFERS` and emit shared/local/temp block counters.
- OR in `INSTRUMENT_WAL` and emit WAL records, full-page images, WAL bytes, and WAL buffer-full counts.
- Add JIT fields when they are available on the executed state.

Possible future work:

- Treat PostgreSQL 18 AIO visibility as a later layer. Start with buffer/I/O timing stats before tracing low-level AIO internals.

### Deparsed Plan Details

`pg_tracing` can deparse plan nodes for richer operation text. `pg_eatrace` currently uses direct plan structs for conservative metadata.

Possible future work:

- Add selected node-specific attributes first, such as bitmap heap exact/lossy pages, sort method, hash bucket/batch information, aggregate strategy, and memoize stats.
- Only add plan deparsing if attributes are no longer enough for Grafana usability.

### SQL-Visible Span Consumption

`pg_tracing` exposes consume/peek views and JSON span functions. `pg_eatrace` intentionally does not expose span payloads through SQL; spans export out-of-band.

Do not add SQL-visible span consumption unless the project goal changes. `pg_eatrace_status()` should remain narrow operational visibility.

## Testing And Release Readiness

Useful tests before `0.1`:

- Extension can be created.
- `pg_eatrace_status()` returns the expected columns.
- `pg_eatrace.collector_url` loads and can be shown.
- OTLP JSON serialization remains covered by focused C tests.
- Error-span JSON remains covered by focused C tests.
- A local collector harness records HTTP requests and validates that a traced successful query produces a query span plus plan-node spans.
- A local collector harness records HTTP requests and validates that a traced failed execution query produces an errored query span with SQLSTATE and message.
- Manual tests cover at least a catalog join, a write query, a recursive CTE, a utility command, a failed utility command, and a query that produces parallel plan nodes if the local cluster can force one.

Avoid tests that depend on exact timing, generated IDs, log order, or collector availability unless the collector is controlled by the test harness.

## Suggested Order

Recommended sequence from here:

1. Validate planner parent-child shape with simple selects, joins, prepared statements, and planning errors.
2. Validate nested queries, functions, triggers, and SPI with manual collector tests.
3. Add SQL privacy controls.
4. Add extension versioning for `0.1`.
5. Add `pg_eatrace.trace_context` as a raw W3C traceparent GUC for transaction-scoped propagation with `SET LOCAL`.
6. Add retry/backoff behavior.
7. Consider executor phase spans behind a default-off GUC.
8. Validate buffer/WAL/JIT attributes against representative workloads.
9. Consider dedicated transaction commit internals if commit latency becomes a useful target.

This keeps the existing architecture intact: trace context enters through SQL comments, query rows return normally, and spans leave out-of-band through the shared-memory queue and background worker.

## References

- DataDog `pg_tracing` README: https://github.com/DataDog/pg_tracing
- DataDog `pg_tracing` reference documentation: https://github.com/DataDog/pg_tracing/blob/main/doc/pg_tracing.md
