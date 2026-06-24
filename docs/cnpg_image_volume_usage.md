# Using pg_eatrace With CloudNativePG Image Volume Extensions

This guide shows how to add `pg_eatrace` to an existing CloudNativePG cluster
when you have a single OCI image URL for the extension.

`pg_eatrace` is a preload extension: the image volume makes the files available
inside the pod, `shared_preload_libraries` loads the hooks and background
worker at PostgreSQL startup, and `CREATE EXTENSION` installs the SQL function
used for status checks.

## Requirements

Use an extension image that matches the PostgreSQL operand image on:

- PostgreSQL major version
- operating system distribution
- CPU architecture

For example, an image built for PostgreSQL 18 on Debian trixie is compatible
with a PostgreSQL 18 Debian trixie operand such as:

```yaml
imageName: ghcr.io/cloudnative-pg/postgis:18.3-3.6.2-system-trixie
```

CloudNativePG mounts image-volume extensions at `/extensions/<name>`. A
standard extension image layout is:

```text
/share/extension/pg_eatrace.control
/share/extension/pg_eatrace--0.0.sql
/lib/pg_eatrace.so
```

With that layout, CloudNativePG's default paths are enough:

- `/extensions/pg_eatrace/share` is added to `extension_control_path`
- `/extensions/pg_eatrace/lib` is added to `dynamic_library_path`

## Add The Extension Image To The Cluster

Patch your `Cluster` manifest under `spec.postgresql`.

```yaml
apiVersion: postgresql.cnpg.io/v1
kind: Cluster
metadata:
  name: my-cluster
spec:
  imageName: ghcr.io/cloudnative-pg/postgis:18.3-3.6.2-system-trixie

  postgresql:
    shared_preload_libraries:
      - pg_eatrace

    extensions:
      - name: pg_eatrace
        image:
          reference: ghcr.io/YOUR_ORG/pg_eatrace:0.0-pg18-trixie

    parameters:
      pg_eatrace.collector_url: "http://otel-collector.observability.svc:4318/v1/traces"
      pg_eatrace.enabled: "on"
      pg_eatrace.span_generation: "top-level"
      pg_eatrace.sample_rate: "1.0"
```

Replace `ghcr.io/YOUR_ORG/pg_eatrace:0.0-pg18-trixie` with the extension image
URL you were given.

Adding or changing an image-volume extension causes CloudNativePG to roll the
PostgreSQL pods so the image can be mounted at startup.

## Install The SQL Extension In A Database

The preload library is global to the PostgreSQL instance, but the SQL objects
are database-scoped. Install them with a CloudNativePG `Database` resource:

```yaml
apiVersion: postgresql.cnpg.io/v1
kind: Database
metadata:
  name: my-cluster-app
spec:
  cluster:
    name: my-cluster
  name: app
  owner: app
  extensions:
    - name: pg_eatrace
      ensure: present
```

This creates the `pg_eatrace_status()` function in the `app` database.

## Configure pg_eatrace

Set `pg_eatrace` GUCs in the `Cluster` manifest under:

```yaml
spec:
  postgresql:
    parameters:
      pg_eatrace.some_setting: "value"
```

Example:

```yaml
postgresql:
  parameters:
    pg_eatrace.collector_url: "http://otel-collector.observability.svc:4318/v1/traces"
    pg_eatrace.enabled: "on"
    pg_eatrace.span_generation: "top-level"
    pg_eatrace.sample_rate: "1.0"
    pg_eatrace.export_interval_ms: "1000"
    pg_eatrace.max_export_batch: "500"
    pg_eatrace.max_spans: "5000"
    pg_eatrace.parallel_worker_spans: "off"
```

Available settings:

- `pg_eatrace.collector_url`: OTLP/HTTP trace endpoint. Only plain `http://`
  URLs are accepted. Empty means the worker drains spans and logs them instead
  of exporting.
- `pg_eatrace.enabled`: master backend kill switch. When `off`, no backend
  spans are generated.
- `pg_eatrace.span_generation`: `none`, `top-level`, or `all`.
  `top-level` traces only top-level statements. `all` also lets nested
  statements inherit the active `pg_eatrace` span as parent.
- `pg_eatrace.sample_rate`: deterministic trace-id based downsampling from
  `0.0` to `1.0`. It only downsamples caller-sampled traces and never overrides
  an upstream unsampled decision.
- `pg_eatrace.export_interval_ms`: background worker wake/export interval.
- `pg_eatrace.max_export_batch`: maximum spans per OTLP HTTP request.
- `pg_eatrace.max_spans`: shared-memory ring capacity. New spans are dropped
  when the ring is full. Requires restart because it sizes shared memory.
- `pg_eatrace.parallel_worker_spans`: emits optional per-worker child spans for
  parallel queries when enabled.

Restart-sensitive settings:

- `shared_preload_libraries` must include `pg_eatrace` at server start.
- `pg_eatrace.max_spans` is `PGC_POSTMASTER`, so changing it requires restart.
- `collector_url`, `export_interval_ms`, and `max_export_batch` are SIGHUP
  settings and can be picked up by the worker after configuration reload.

## Verify The Mount And PostgreSQL Settings

Check the resolved extension image configuration:

```bash
kubectl get cluster my-cluster \
  -o jsonpath='{.status.pgDataImageInfo.extensions}{"\n"}'
```

Check the mounted files:

```bash
kubectl exec my-cluster-1 -c postgres -- \
  ls -l /extensions/pg_eatrace/share/extension /extensions/pg_eatrace/lib
```

Check PostgreSQL path and preload settings:

```bash
kubectl exec my-cluster-1 -c postgres -- \
  psql -d app -c "SHOW shared_preload_libraries; SHOW extension_control_path; SHOW dynamic_library_path;"
```

Expected shape:

```text
shared_preload_libraries = pg_eatrace
extension_control_path includes /extensions/pg_eatrace/share
dynamic_library_path includes /extensions/pg_eatrace/lib
```

Check runtime status:

```bash
kubectl exec my-cluster-1 -c postgres -- \
  psql -d app -c "SELECT * FROM pg_eatrace_status();"
```

## Generate A Test Span

Run a statement with a SQLCommenter-style W3C traceparent comment:

```sql
/*traceparent='00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01'*/ SELECT 42;
```

Then check status again:

```bash
kubectl exec my-cluster-1 -c postgres -- \
  psql -d app -c "SELECT queue_depth, exported_spans, export_failures, last_export_error FROM pg_eatrace_status();"
```

If the collector is reachable, `exported_spans` should increase. If it is not
reachable, `export_failures` and `last_export_error` should show the connection
or HTTP error. Query results still return normally either way.

## Traceparent Behavior

`pg_eatrace` reads trace context from SQL block comments:

```sql
/*traceparent='00-<trace_id>-<parent_span_id>-01'*/ SELECT ...;
```

Invalid traceparents create no spans and do not fail the query. If multiple
comments are present, the parser scans comments left to right and uses the first
syntactically valid traceparent it finds. The W3C sampled bit must be set.

For transaction workloads, include a traceparent on each statement you want
traced. A traceparent on one statement is not stored as transaction-local state
for later client round trips.

## References

- CloudNativePG image volume extensions:
  https://cloudnative-pg.io/docs/1.29/imagevolume_extensions/
- CloudNativePG declarative database management:
  https://cloudnative-pg.io/docs/1.29/declarative_database_management/
