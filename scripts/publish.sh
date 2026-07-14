#!/usr/bin/env bash
# publish.sh — CLI-native image publishing for RawImport Pipeline.
#
# Builds the image locally and pushes to BOTH registries:
#   - GHCR:      ghcr.io/rawrdoge/autodngconverter   (always)
#   - Docker Hub: rawrdoge/autodngconverter          (if creds present)
#
# Usage:
#   ./scripts/publish.sh [tag]      # tag defaults to 'latest'
#
# Credentials (any of these work):
#   GHCR:       docker login ghcr.io   (uses $GITHUB_ACTOR / $GITHUB_TOKEN)
#   Docker Hub: docker login           (uses $DOCKERHUB_USERNAME / $DOCKERHUB_TOKEN)
#               or export DOCKERHUB_USERNAME / DOCKERHUB_TOKEN before running.
#
# The script loads ./.env if present (for local runs) but never commits it.
set -euo pipefail

TAG="${1:-latest}"
REPO="rawrdoge/autodngconverter"
GHCR_IMAGE="ghcr.io/${REPO}"
HUB_IMAGE="${REPO}"

# Load local .env if present (no-op if missing).
if [[ -f ./.env ]]; then
  set -a
  # shellcheck disable=SC1091
  source ./.env
  set +a
fi

echo "==> Building ${GHCR_IMAGE}:${TAG} (and tagging ${HUB_IMAGE}:${TAG})"
docker build \
  --build-arg "VERSION=${TAG}" \
  -t "${GHCR_IMAGE}:${TAG}" \
  -t "${HUB_IMAGE}:${TAG}" \
  .

echo "==> Pushing to GHCR"
docker push "${GHCR_IMAGE}:${TAG}"

# Docker Hub is optional: only push if the user is logged in / creds are set.
if docker info 2>/dev/null | grep -q "Username: ${DOCKERHUB_USERNAME:-none}" \
   || [[ -n "${DOCKERHUB_TOKEN:-}" ]]; then
  echo "==> Pushing to Docker Hub (${HUB_IMAGE}:${TAG})"
  docker push "${HUB_IMAGE}:${TAG}"
else
  echo "==> Skipping Docker Hub: not logged in (run 'docker login' or set DOCKERHUB_USERNAME/DOCKERHUB_TOKEN)."
fi

echo "==> Done. Pull with:"
echo "    docker pull ${GHCR_IMAGE}:${TAG}"
echo "    docker pull ${HUB_IMAGE}:${TAG}"