#!/usr/bin/env bash
#
# Build file_browser_gba.gba with the official devkitPro devkitARM Docker image.
# Requires only Docker (no local toolchain). This project is self-contained —
# the shared hardware/FS layer is vendored into ./lib and ./source — so the
# container only needs THIS folder mounted.
#
set -euo pipefail
IMG="devkitpro/devkitarm:20241104"

echo ">> Building File-Browser-GBA with $IMG ..."
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -v "$(pwd)":/project \
  "$IMG" \
  bash -c 'cd /project && make rebuild'

echo ">> Done. Output: $(pwd)/file_browser_gba.gba"
