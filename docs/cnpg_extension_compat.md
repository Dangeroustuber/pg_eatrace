# Fixing an extension / operand image mismatch

The `pg_eatrace` extension image ships a compiled `pg_eatrace.so`. That library
is `dlopen`'d **inside the PostgreSQL operand container**, so it must match the
operand image on three axes — never the Kubernetes nodes:

1. **PostgreSQL major version** (18)
2. **CPU architecture** (amd64 / arm64)
3. **Distro / glibc** (trixie, bookworm, …)

If any axis is off, the extension fails to load. This is how you find the
mismatch and fix it.

## 1. Find what the operand needs (source of truth)

The operand is whatever your `Cluster` runs, not what the nodes run:

```bash
# Image the cluster is actually using:
kubectl get cluster <name> -o jsonpath='{.spec.imageName}{"\n"}'
# (or it resolves through .spec.imageCatalogRef)
```

Read the three axes straight off a running pod:

```bash
kubectl exec -it <cluster>-1 -c postgres -- bash -c \
  'cat /etc/os-release | grep VERSION_CODENAME; \
   postgres --version; \
   uname -m; \
   ldd --version | head -1'
```

`VERSION_CODENAME` → distro, `postgres --version` → PG major, `uname -m` →
arch, `ldd --version` → glibc version your `.so` must not exceed.

## 2. Pre-flight check (catch it BEFORE deploying)

Cheapest possible test: load the `.so` the way Postgres will, against the exact
operand image. No cluster, no push needed.

```bash
OPERAND=ghcr.io/cloudnative-pg/postgresql:18-standard-trixie   # match your Cluster

docker run --rm -v "$(pwd)/pg_eatrace.so:/tmp/pg_eatrace.so:ro" "$OPERAND" \
  bash -c 'ldd /tmp/pg_eatrace.so'
```

- Clean output (every line resolves) → compatible.
- `version 'GLIBC_2.xx' not found` → distro/glibc mismatch (see below).
- `Exec format error` / `wrong architecture` → arch mismatch.

## 3. Fix by symptom

### Distro / glibc mismatch
Postgres log: `could not load library ".../pg_eatrace.so": /lib/...: version 'GLIBC_2.38' not found`

Rebuild against the operand's distro. One-line change in the `Dockerfile`:

```dockerfile
FROM debian:bookworm-slim AS build      # was debian:trixie-slim
```

Then build and tag to match, and point the manifest at the new tag:

```bash
docker build -t ghcr.io/dangeroustuber/pg_eatrace:0.0-pg18-bookworm .
docker push   ghcr.io/dangeroustuber/pg_eatrace:0.0-pg18-bookworm
```

(Rule of thumb: build glibc ≤ operand glibc. Building on the *older* distro is
forward-compatible; the reverse is not.)

### PostgreSQL major mismatch
Postgres log: `incompatible library ".../pg_eatrace.so": missing magic block`
(or a version-mismatch magic-block error).

The `.so` was built for a different PG major. In the `Dockerfile` build stage,
install the matching `-dev` package and point `make` at its `pg_config`:

```dockerfile
RUN apt-get install -y --no-install-recommends postgresql-server-dev-17   # match operand
RUN make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config ...
```

Retag `…-pg17-…` and push.

### CPU architecture mismatch
Pod won't start; events show `exec format error`, or the image volume's `.so`
is for the wrong arch.

Build for the operand's arch (or both) with buildx:

```bash
docker buildx build --platform linux/amd64,linux/arm64 \
  -t ghcr.io/dangeroustuber/pg_eatrace:0.0-pg18-trixie --push .
```

A multi-arch manifest lets the node pull the right one automatically.

## 4. Why tags carry the full triple

Images are tagged `0.0-pg18-trixie`, not just `0.0`, precisely so the
distro/major/arch combos can coexist in one repo and the `Cluster` manifest
references whichever matches its operand. When in doubt, name the operand you
built against in the tag.

## Note on dependencies

`pg_eatrace.so` links **libc only** (the HTTP exporter is hand-rolled, no
libcurl), so glibc version is the entire distro-compatibility surface. There are
no bundled `.so` dependencies to chase, and `ld_library_path` is never needed.
