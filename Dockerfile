# syntax=docker/dockerfile:1

FROM debian:12-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY Makefile ./
COPY src ./src

RUN mkdir -p data && make && strip /src/sql_processor

FROM builder AS test

COPY tests ./tests

FROM debian:12-slim AS runtime

RUN useradd --system --create-home --home-dir /app --shell /usr/sbin/nologin appuser

WORKDIR /app

COPY --from=builder /src/sql_processor ./sql_processor

RUN mkdir -p /app/data && chown -R appuser:appuser /app

USER appuser

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD test -r /proc/1/stat

CMD ["./sql_processor", "--server", "8080"]
