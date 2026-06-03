#ifndef FS_OPS_H
#define FS_OPS_H

/*
 * Generic file-system operations for the SD file browser, layered on FatFs.
 *
 * Pure C: this file and fs_ops.c include only <stdint.h>/<stdbool.h>/<string.h>
 * and FatFs's ff.h — NO tonc/GBA headers — so the list/sort/freespace logic can
 * be dual-compiled and unit-tested on the host against a FatFs image (mirrors
 * the record-mixer's pure-core convention). The caller owns all big buffers
 * (the FsEntry array lives in the UI layer's EWRAM, passed in here).
 */

#include <stdint.h>
#include <stdbool.h>
#include "ff.h"

#define FS_NAME_CAP 256   /* fits a 255-byte FatFs LFN (FF_LFN_BUF) + NUL */

typedef struct {
  char     name[FS_NAME_CAP];  /* UTF-8 long name (FF_LFN_UNICODE 2)        */
  bool     is_dir;             /* AM_DIR                                    */
  uint8_t  attrib;             /* raw FILINFO.fattrib (AM_RDO/HID/SYS/ARC)  */
  uint64_t size;              /* file size in bytes (0 for directories)    */
  uint32_t dosdt;             /* (fdate << 16) | ftime — DOS date+time     */
} FsEntry;

typedef enum { FS_SORT_NAME = 0, FS_SORT_SIZE = 1, FS_SORT_DATE = 2 } FsSortKey;

/* List directory `dir` into out[0..max-1] (directories included, is_dir=true).
 * Returns the number of entries placed (>= 0), or -1 if the directory could
 * not be opened. If there are more than `max` entries the list is capped at
 * `max` and *truncated (if non-NULL) is set true — never a silent cap. */
int fsop_list(const char* dir, FsEntry* out, int max, bool* truncated, bool show_hidden);

/* Stable in-place sort. Directories always sort before files (regardless of
 * `reverse`); within each group the order is by `key`, with name as the
 * tiebreaker. `reverse` flips the within-group key order only. */
void fsop_sort(FsEntry* a, int n, FsSortKey key, bool reverse);

/* Free / total bytes of the volume that contains `path` (via f_getfree).
 * Either out pointer may be NULL. Returns FR_OK or the FatFs error. */
FRESULT fsop_freespace(const char* path, uint64_t* free_bytes,
                       uint64_t* total_bytes);

#define FS_PATH_CAP         256   /* max path length the recursive ops handle */
#define FS_RMTREE_MAX_DEPTH 24    /* max directory nesting for recursive walks */

/* Recursively total the bytes + file/subfolder counts under directory `path`
 * (excluding `path` itself). Uses the same explicit dir-stack as the copy/delete
 * walks — never C recursion — bounded by FS_RMTREE_MAX_DEPTH. Any out pointer
 * may be NULL. Returns FR_OK, or the first FatFs error (FR_NOT_ENOUGH_CORE if
 * the tree is too deep or a path exceeds FS_PATH_CAP). Read-only: safe on both
 * carts. */
FRESULT fsop_dirsize(const char* path, uint64_t* bytes, uint32_t* files,
                     uint32_t* dirs);

/* Recursively search `root` and every subdirectory (bounded by
 * FS_RMTREE_MAX_DEPTH) for entries whose name contains `keyword` (ASCII
 * case-insensitive substring; empty keyword matches everything). Writes up to
 * `max` full paths into out_paths[][FS_PATH_CAP] with a parallel out_isdir[]
 * (1 = directory). Sets *truncated (if non-NULL) when more than `max` matched.
 * Unreadable/too-deep subtrees — and any entry whose full path would exceed
 * FS_PATH_CAP — are silently skipped (not fatal, not flagged). Read-only — safe
 * on both carts. Returns the match count (>= 0), or -1 if `root` cannot be opened. */
int fsop_find(const char* root, const char* keyword,
              char out_paths[][FS_PATH_CAP], uint8_t* out_isdir,
              int max, bool* truncated);

/* ---- mutating ops (Phase 1) — callers gate these on EZ-Flash Omega ------ */

/* Create a directory. Returns FR_OK, FR_EXIST, etc. */
FRESULT fsop_mkdir(const char* path);

/* Rename / move within the same volume. Returns FR_EXIST if `newpath` already
 * exists (f_rename does not overwrite). */
FRESULT fsop_rename(const char* oldpath, const char* newpath);

/* Set/clear FAT attribute bits: f_chmod(path, set, mask) — e.g. toggle AM_RDO
 * by passing mask=AM_RDO and set = AM_RDO (on) or 0 (off). */
FRESULT fsop_chmod(const char* path, uint8_t set, uint8_t mask);

/* Copy a file, or an entire directory tree, from `src` to a NEW path `dst`.
 * Directories are copied recursively using the same explicit-stack walk as the
 * delete (no C recursion). Existing destination files are overwritten
 * (FA_CREATE_ALWAYS); the caller is responsible for confirm-overwrite and for
 * refusing to copy a directory into its own subtree. Returns FR_OK or the first
 * FatFs error (FR_DENIED on a short write = disk full). For a CUT/move within
 * the volume use fsop_rename instead — it is atomic and needs no copy. */
FRESULT fsop_copy(const char* src, const char* dst);

/* One byte edit for the hex editor: set byte at `off` to `val`. */
typedef struct { uint32_t off; uint8_t val; } HexEdit;

/* Apply `n` byte edits to `path` SAFELY (the highest-risk op, so it never edits
 * in place): stream the original into a temp with the edits applied, then
 * byte-verify the temp equals original-with-edits, then back up the original to
 * "<path>.bak~" and atomically rename the temp over it. On any failure the
 * original is left untouched (temp removed), or restored from the backup if the
 * final rename fails. File size is preserved (edits must be at offsets < size).
 * Returns FR_OK, or FR_INT_ERR if the written temp fails verification. */
FRESULT fsop_apply_edits(const char* path, const HexEdit* edits, int n);

/* Delete a file or an entire directory tree. For a non-empty directory it
 * recursively removes the contents first, using an EXPLICIT stack (never C
 * recursion — the GBA IWRAM stack is only 32 KiB) bounded by
 * FS_RMTREE_MAX_DEPTH, and clears AM_RDO on items as needed. Refuses to delete
 * the volume root. Returns FR_OK or the first FatFs error encountered
 * (FR_NOT_ENOUGH_CORE if the tree is too deep or a path exceeds FS_PATH_CAP). */
FRESULT fsop_delete(const char* path);

#endif /* FS_OPS_H */
