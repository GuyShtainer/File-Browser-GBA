/* Force-included (-include) into source/fs_ops.c by tests/run_host.sh only.
 * 1. fs_ops.c tags its big buffers section(".sbss") for the GBA linker; Mach-O rejects
 *    that string, so the attribute argument is macro'd away on the host.
 * 2. f_open / f_rename / f_unlink are routed through wrap_* (defined in
 *    host_save_test.c) so a test can make the Nth matching call fail, and can tamper
 *    with the temp file between its write and its verify read-back.
 * ff.h and the libc headers are pulled in FIRST so the macros below never touch them. */
#ifndef FB_WRAP_H
#define FB_WRAP_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ff.h"
#define section(x)
FRESULT wrap_f_open(FIL* fp, const TCHAR* path, BYTE mode);
FRESULT wrap_f_rename(const TCHAR* a, const TCHAR* b);
FRESULT wrap_f_unlink(const TCHAR* path);
#define f_open   wrap_f_open
#define f_rename wrap_f_rename
#define f_unlink wrap_f_unlink
#endif
