#!/usr/bin/env bash
#
# Build sd_browser.gba with the official devkitPro devkitARM Docker image.
# Requires only Docker. Mounts the REPO ROOT (so the ../../lib and ../../source
# shared dirs resolve inside the container) and builds in this tool's folder.
#
set -euo pipefail
IMG="devkitpro/devkitarm:20241104"

# Repo root = two levels up from this script (projects/sd-browser -> repo root).
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

echo ">> Building sd-browser with $IMG ..."
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -v "$ROOT":/project \
  "$IMG" \
  bash -c 'cd /project/projects/sd-browser && make rebuild'

echo ">> Done. Output: $ROOT/projects/sd-browser/sd_browser.gba"
