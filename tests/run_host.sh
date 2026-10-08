#!/bin/sh
# Host tests for the pure-C cores. Usage: tests/run_host.sh
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/fb-host-$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
CF="-std=c99 -Wall -Wextra -Werror -O1"
cc $CF -o "$OUT/text" host_text_test.c ../source/textbuf.c
"$OUT/text"
