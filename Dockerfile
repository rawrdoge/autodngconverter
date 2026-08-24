# Portable-branch container build (optional target — the primary artifact is
# the Windows ZIP). SQLite-only: no MariaDB, no external DB server.
# dnglab is built from the vibelabdng submodule (Rust).

# ============================================================
# Stage 0: Build dnglab (Rust) from vibelabdng submodule
# ============================================================
FROM rust:1.89-bookworm AS dnglab-build
WORKDIR /vibelabdng
COPY vibelabdng/Cargo.toml vibelabdng/Cargo.lock ./
COPY vibelabdng/bin ./bin
COPY vibelabdng/rawler ./rawler
RUN cargo build --release --bin dnglab

# ============================================================
# Stage 1: Build C++ app (rawimport-pipeline)
# ============================================================
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++-12 pkg-config make \
    libssl-dev libsqlite3-dev libspdlog-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN mkdir -p build && cd build \
    && cmake -DCMAKE_CXX_COMPILER=g++-12 -DCMAKE_BUILD_TYPE=Release .. \
    && cmake --build . -j"$(nproc)"

# ============================================================
# Stage 2: Runtime
# ============================================================
FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libsqlite3-0 exiftool ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -u 10001 -m appuser

COPY --from=build /src/build/rawimport-pipeline /usr/local/bin/
COPY --from=dnglab-build /vibelabdng/target/release/dnglab /usr/local/bin/

ENV WATCH_DIR=/watch
ENV OUTPUT_DIR=/output
ENV ARCHIVE_DIR=/archive
ENV DB_NAME=/data/rawimport.db

VOLUME ["/watch", "/output", "/archive", "/data"]
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

USER appuser
ENTRYPOINT ["/usr/local/bin/rawimport-pipeline"]