CREATE FUNCTION pg_eatrace_status(
    OUT queue_depth integer,
    OUT queue_capacity integer,
    OUT dropped_spans bigint,
    OUT exported_spans bigint,
    OUT export_failures bigint,
    OUT dequeued_spans bigint,
    OUT last_export_error text,
    OUT last_http_status_code bigint,
    OUT last_successful_export_at timestamptz,
    OUT last_failed_export_at timestamptz
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_eatrace_status'
LANGUAGE C PARALLEL SAFE;
