#!/bin/sh
# Builds the host-side half of the virtual-SD harness and runs its test.
#   tools/build_vsd_host.sh [LIB_DIR]
# LIB_DIR: the lib/ whose fatfs the image factory links (default: this repo's lib/). A
# sub-project that vendored lib/ + tests/hostfat + tools/ passes nothing and runs its copy.
# Produces /tmp/vsd_img (the CLI) and /tmp/hvsdimg (the test, run here). Exits non-zero
# on any compile warning-as-error, test failure, or CRC mismatch.
#
# FF_USE_MKFS=1 is a HOST-ONLY override via -D: the GBA ffconf.h keeps f_mkfs off (a d-pad
# format command is a data-loss trap), and this script never edits ffconf.h. ffconf.h
# guards its definition with a plain #define, so the override is applied to a private
# copy of lib/fatfs in a temp dir (nothing under lib/ is touched).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIB="${1:-$ROOT/lib}"
OUT="${TMPDIR:-/tmp}/vsd-host-$$"
mkdir -p "$OUT/fatfs"
trap 'rm -rf "$OUT"' EXIT
cp "$LIB"/fatfs/*.c "$LIB"/fatfs/*.h "$OUT/fatfs/"
sed 's/^#define FF_USE_MKFS[[:space:]].*/#define FF_USE_MKFS 1/' "$LIB/fatfs/ffconf.h" > "$OUT/fatfs/ffconf.h"
grep -q '^#define FF_USE_MKFS 1' "$OUT/fatfs/ffconf.h" || { echo "FF_USE_MKFS override failed" >&2; exit 1; }
CF="-std=gnu11 -Wall -Wextra -Werror -O1 -Dsiprintf=sprintf -I $ROOT/tests/hostfat -I $OUT/fatfs -I $ROOT/tools"
SRC="$ROOT/tools/vsd_img.c $ROOT/tools/host_walk.c $ROOT/tools/crc32.c $OUT/fatfs/ff.c $OUT/fatfs/ffunicode.c $ROOT/tests/hostfat/ramdisk.c"
cc $CF -o /tmp/vsd_img $SRC
cc $CF -DVSD_IMG_NO_MAIN -o /tmp/hvsdimg "$ROOT/tests/host_vsdimg_test.c" $SRC
/tmp/hvsdimg
echo "build_vsd_host: OK (/tmp/vsd_img built, host test green)"
