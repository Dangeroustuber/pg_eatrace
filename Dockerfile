# Builds the pg_eatrace extension image for CloudNativePG image-volume
# extensions. The final image contains only the extension files in the layout
# CNPG expects (share/extension + lib); it is not a runnable container.
#
# The build stage must match the operand image's PostgreSQL major version,
# distribution, and architecture (PostgreSQL 18, Debian trixie, amd64).

FROM debian:trixie-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        postgresql-common ca-certificates build-essential \
    && /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y \
    && apt-get install -y --no-install-recommends postgresql-server-dev-18

COPY . /src
WORKDIR /src

RUN make PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config \
    && mkdir -p /out/share/extension /out/lib \
    && cp pg_eatrace.so /out/lib/ \
    && cp pg_eatrace.control pg_eatrace--0.0.sql /out/share/extension/

FROM scratch
COPY --from=build /out/ /
