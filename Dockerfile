# RawImport Pipeline — container image
# Multi-stage: build the dnglab converter (Rust), build the Go binary,
# run on a slim Debian base with both baked in.
FROM rust:1-bookworm AS dnglab
WORKDIR /build
COPY vibelabdng/ ./
RUN cargo build --release \
    && strip target/release/dnglab

FROM golang:1.26-bookworm AS gobuild
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -o /out/rawimport-pipeline .

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl exiftool \
    && rm -rf /var/lib/apt/lists/*

COPY --from=dnglab /build/target/release/dnglab /usr/local/bin/dnglab
COPY --from=gobuild /out/rawimport-pipeline /usr/local/bin/rawimport-pipeline

VOLUME ["/watch", "/output", "/archive", "/db"]
ENV WATCH_DIR=/watch \
    OUTPUT_DIR=/output \
    ARCHIVE_DIR=/archive \
    DB_DRIVER=mariadb \
    DB_HOST=mariadb \
    DB_PORT=3306 \
    CONVERTER_ENGINE=dnglab \
    DNGLAB_BIN=/usr/local/bin/dnglab \
    EXIFTOOL_BIN=exiftool \
    PORT=8080

HEALTHCHECK --interval=30s --timeout=10s --start-period=30s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/rawimport-pipeline"]
