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
cc $CF -o "$OUT/pathlist" host_pathlist_test.c ../source/pathlist.c
"$OUT/pathlist"
# fsop_save_buffer over a RAM disk: the REAL fs_ops.c + ff.c. The app's ffconf has no
# f_mkfs, so a private copy of lib/fatfs gets FF_USE_MKFS=1 (nothing else changes).
mkdir -p "$OUT/fatfs"
cp ../lib/fatfs/*.c ../lib/fatfs/*.h "$OUT/fatfs/"
sed 's/^#define FF_USE_MKFS[[:space:]].*/#define FF_USE_MKFS 1/' ../lib/fatfs/ffconf.h > "$OUT/fatfs/ffconf.h"
SI="-std=gnu99 -Wall -Wextra -O1 -I hostfat -I $OUT/fatfs -I ../source"
cc $SI -include hostfat/wrap.h -c -o "$OUT/fs_ops.o" ../source/fs_ops.c   # wrap.h only for this one file
cc $SI -o "$OUT/save" host_save_test.c hostfat/ramdisk.c "$OUT/fs_ops.o" "$OUT/fatfs/ff.c" "$OUT/fatfs/ffunicode.c"
"$OUT/save"
