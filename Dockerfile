# Multi-stage build for the RawImport Pipeline C++20 rewrite.
# Toolchain image: cmake + g++-12 + libs. Runtime: debian:bookworm-slim.
# Non-destructive: this is Dockerfile.cpp, separate from the Go Dockerfile.
# See PRD_RawImport_Pipeline_CppRewrite.md §10 and ORCHESTRATION_CppRewrite.md §2.4.

# ---- Stage 1: build ----
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++-12 pkg-config make \
    libssl-dev libmariadb-dev nlohmann-json3-dev libspdlog-dev \
    libcxxopts-dev \
    ca-certificates exiftool libexif12 \
    && rm -rf /var/lib/apt/lists/*
# NOTE: dnglab binary (from vibelabdng submodule) is copied from a build artifact
# or mounted at runtime; not fetched here to keep the base image Adobe-free.

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY migrations ./migrations
RUN mkdir -p build && cd build \
    && cmake -DCMAKE_CXX_COMPILER=g++-12 -DCMAKE_BUILD_TYPE=Release .. \
    && cmake --build . -j"$(nproc)"

# ---- Stage 2: runtime ----
FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libmariadb3 libspdlog1.10 \
    ca-certificates exiftool libexif12 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -u 10001 -m appuser

WORKDIR /app
COPY --from=build /src/build/rawimport-pipeline /usr/local/bin/
# Migrations land under /db/migrations so the default db_dir=/db resolves them
# (main.cpp uses cfg.db_dir + "/migrations").
COPY migrations /db/migrations/

# Volumes (same as Go service)
VOLUME ["/watch", "/output", "/archive", "/db"]

ENV WATCH_DIR=/watch
ENV OUTPUT_DIR=/output
ENV ARCHIVE_DIR=/archive
ENV DB_DIR=/db

# Health check on the REST endpoint (PRD §8 GET /health)
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

USER appuser
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/rawimport-pipeline"]