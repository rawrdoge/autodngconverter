# Multi-stage build for the RawImport Pipeline C++20 rewrite.
# Toolchain image: cmake + g++-12 + libs. Runtime: debian:bookworm-slim.
# Non-destructive: this is Dockerfile.cpp, separate from the Go Dockerfile.
# See PRD_RawImport_Pipeline_CppRewrite.md §10 and ORCHESTRATION_CppRewrite.md §2.4.

# ============================================================
# Stage 0: Build dnglab (Rust) from vibelabdng submodule
# ============================================================
FROM rust:1.78-bookworm AS dnglab-build
WORKDIR /vibelabdng
# Copy only what's needed for the build (leverage Docker layer cache)
COPY vibelabdng/Cargo.toml vibelabdng/Cargo.lock ./
COPY vibelabdng/bin ./bin
COPY vibelabdng/rawler ./rawler
# Build release binary
RUN cargo build --release --bin dnglab

# ============================================================
# Stage 1: Build C++ app (rawimport-pipeline)
# ============================================================
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++-12 pkg-config make \
    libssl-dev libmariadb-dev nlohmann-json3-dev libspdlog-dev \
    libcxxopts-dev \
    ca-certificates exiftool libexif12 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY migrations ./migrations
# Copy dnglab binary from Stage 0
COPY --from=dnglab-build /vibelabdng/target/release/dnglab /usr/local/bin/dnglab

RUN mkdir -p build && cd build \
    && cmake -DCMAKE_CXX_COMPILER=g++-12 -DCMAKE_BUILD_TYPE=Release .. \
    && cmake --build . -j"$(nproc)"

# ============================================================
# Stage 2: Runtime
# ============================================================
FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libmariadb3 libspdlog1.10 \
    ca-certificates exiftool libexif12 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -u 10001 -m appuser

WORKDIR /app
COPY --from=build /src/build/rawimport-pipeline /usr/local/bin/
COPY --from=build /usr/local/bin/dnglab /usr/local/bin/dnglab
COPY migrations /db/migrations/

# Appdata directory for user-configurable engines & config
RUN mkdir -p /appdata/engines \
    && ln -sf /usr/local/bin/dnglab /appdata/engines/dnglab

# Volumes
VOLUME ["/watch", "/output", "/archive", "/db", "/appdata"]

ENV WATCH_DIR=/watch
ENV OUTPUT_DIR=/output
ENV ARCHIVE_DIR=/archive
ENV DB_DIR=/db
ENV APPDATA_DIR=/appdata

# Health check on the REST endpoint (PRD §8 GET /health)
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

USER appuser
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/rawimport-pipeline"]