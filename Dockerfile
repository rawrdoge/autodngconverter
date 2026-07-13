# RawImport Pipeline — container image (PRD §6.1)
# Multi-stage: build the Go binary, run on a slim Debian base with dnglab.
FROM golang:1.22-bookworm AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -o /out/rawimport-pipeline .

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl exiftool \
    && rm -rf /var/lib/apt/lists/*
# dnglab is fetched at runtime via DNGLAB_BIN or expected on PATH; mount or
# bake it here. For local builds, copy a prebuilt dnglab binary:
# COPY dnglab /usr/local/bin/dnglab
COPY --from=build /out/rawimport-pipeline /usr/local/bin/rawimport-pipeline

VOLUME ["/watch", "/output", "/archive", "/db"]
ENV WATCH_DIR=/watch \
    OUTPUT_DIR=/output \
    ARCHIVE_DIR=/archive \
    DB_DRIVER=mariadb \
    DB_HOST=mariadb \
    DB_PORT=3306 \
    CONVERTER_ENGINE=dnglab \
    EXIFTOOL_BIN=exiftool \
    PORT=8080

HEALTHCHECK --interval=30s --timeout=10s --start-period=30s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/rawimport-pipeline"]