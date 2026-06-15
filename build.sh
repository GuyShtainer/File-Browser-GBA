#!/usr/bin/env bash
#
# Build sd_browser.gba with the official devkitPro devkitARM Docker image.
# Requires only Docker (no local toolchain). This project is self-contained —
# the shared hardware/FS layer is vendored into ./lib and ./source — so the
# container only needs THIS folder mounted.
#
set -euo pipefail
IMG="devkitpro/devkitarm:20241104"

echo ">> Building sd-browser with $IMG ..."
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -v "$(pwd)":/project \
  "$IMG" \
  bash -c 'cd /project && make rebuild'

echo ">> Done. Output: $(pwd)/sd_browser.gba"
