#include "fs_ops.h"
#include <string.h>

/* ASCII case-insensitive compare (locale-free, host-portable). */
static int name_ci(const char* a, const char* b) {
  for (;;) {
    unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
    if (x >= 'a' && x <= 'z') x = (unsigned char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (unsigned char)(y - 32);
    if (x != y) return (int)x - (int)y;
    if (!x) return 0;
  }
}

int fsop_list(const char* dir, FsEntry* out, int max, bool* truncated) {
  if (truncated) *truncated = false;
  if (max <= 0) return 0;

  DIR d;
  FILINFO fno;
  if (f_opendir(&d, dir) != FR_OK) return -1;

  int n = 0;
  /* End of directory is signalled by an empty fname[0] WITHOUT an error; a
   * non-FR_OK return is a real I/O error (EZ-Flash reads do not retry), which
   * must NOT be mistaken for a short/clean listing — capture fr to tell them
   * apart. */
  FRESULT fr;
  while ((fr = f_readdir(&d, &fno)) == FR_OK && fno.fname[0]) {
    if (n >= max) { if (truncated) *truncated = true; break; }

    FsEntry* e = &out[n];
    int i = 0;
    for (; i < FS_NAME_CAP - 1 && fno.fname[i]; i++) e->name[i] = fno.fname[i];
    e->name[i] = 0;

    e->is_dir = (fno.fattrib & AM_DIR) != 0;
    e->attrib = fno.fattrib;
    e->size   = e->is_dir ? 0u : (uint64_t)fno.fsize;
    e->dosdt  = ((uint32_t)fno.fdate << 16) | (uint32_t)fno.ftime;
    n++;
  }
  f_closedir(&d);
  /* fr is FR_OK both at clean EOD and after the n>=max break (last readdir
   * succeeded); only a genuine error makes it non-OK. */
  if (fr != FR_OK) return -1;
  return n;
}

/* <0 if x should sort before y, >0 after, 0 equal. */
static int entry_cmp(const FsEntry* x, const FsEntry* y, FsSortKey key,
                     bool reverse) {
  /* Directories always come first — this grouping is NOT reversible. */
  if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;

  int c = 0;
  switch (key) {
    case FS_SORT_SIZE:
      c = (x->size < y->size) ? -1 : (x->size > y->size) ? 1 : 0;
      break;
    case FS_SORT_DATE:
      c = (x->dosdt < y->dosdt) ? -1 : (x->dosdt > y->dosdt) ? 1 : 0;
      break;
    case FS_SORT_NAME:
    default:
      c = name_ci(x->name, y->name);
      break;
  }
  if (c == 0) c = name_ci(x->name, y->name);  /* stable-ish tiebreak by name */
  return reverse ? -c : c;
}

void fsop_sort(FsEntry* a, int n, FsSortKey key, bool reverse) {
  /* Insertion sort: n is bounded (a directory listing) and this runs once per
   * scan / sort-toggle, never inside an SD transfer. */
  for (int i = 1; i < n; i++) {
    FsEntry tmp = a[i];
    int j = i - 1;
    while (j >= 0 && entry_cmp(&a[j], &tmp, key, reverse) > 0) {
      a[j + 1] = a[j];
      j--;
    }
    a[j + 1] = tmp;
  }
}

FRESULT fsop_freespace(const char* path, uint64_t* free_bytes,
                       uint64_t* total_bytes) {
  DWORD nclst = 0;
  FATFS* fs = 0;
  FRESULT fr = f_getfree(path, &nclst, &fs);
  if (fr != FR_OK) return fr;

  /* Sector size is fixed at 512 here (FF_MIN_SS == FF_MAX_SS == 512), so the
   * SS() macro is a constant and FATFS carries no ssize field. */
  uint64_t cluster_bytes = (uint64_t)fs->csize * 512u;
  if (free_bytes)  *free_bytes  = (uint64_t)nclst * cluster_bytes;
  if (total_bytes) *total_bytes = (uint64_t)(fs->n_fatent - 2) * cluster_bytes;
  return FR_OK;
}

/* ---- mutating ops ------------------------------------------------------- */

FRESULT fsop_mkdir(const char* path) { return f_mkdir(path); }

FRESULT fsop_rename(const char* oldpath, const char* newpath) {
  return f_rename(oldpath, newpath);
}

FRESULT fsop_chmod(const char* path, uint8_t set, uint8_t mask) {
  return f_chmod(path, (BYTE)set, (BYTE)mask);
}

/* Explicit-stack directory tree removal. One open DIR per nesting level lives
 * in the fixed static stack below (in .bss, ~2 KiB), so depth is bounded and
 * the IWRAM stack frame stays tiny — we never use C recursion. FatFs f_readdir
 * never yields "." or "..", so the walk can never escape the target subtree. */
static DIR     s_ds[FS_RMTREE_MAX_DEPTH];
static int     s_plen[FS_RMTREE_MAX_DEPTH];
static char    s_path[FS_PATH_CAP];
static FILINFO s_fno;

static FRESULT rmtree(const char* root) {
  int n = 0;
  for (; root[n] && n < FS_PATH_CAP - 1; n++) s_path[n] = root[n];
  s_path[n] = 0;
  if (n == 0 || (n == 1 && s_path[0] == '/')) return FR_INVALID_NAME;  /* never the root */

  int depth = 0;
  FRESULT fr = f_opendir(&s_ds[0], s_path);
  if (fr != FR_OK) return fr;
  s_plen[0] = n;

  while (depth >= 0) {
    fr = f_readdir(&s_ds[depth], &s_fno);
    if (fr != FR_OK) break;

    if (s_fno.fname[0] == 0) {              /* level exhausted: remove this dir */
      f_closedir(&s_ds[depth]);
      s_path[s_plen[depth]] = 0;            /* s_path == this directory's path  */
      fr = f_unlink(s_path);
      if (fr == FR_DENIED) { f_chmod(s_path, 0, AM_RDO); fr = f_unlink(s_path); }
      depth--;
      if (fr != FR_OK) break;
      if (depth >= 0) s_path[s_plen[depth]] = 0;  /* restore parent path */
      continue;
    }

    int pl = s_plen[depth];
    int nl = (int)strlen(s_fno.fname);
    if (pl + 1 + nl + 1 > FS_PATH_CAP) { fr = FR_NOT_ENOUGH_CORE; break; }
    if (pl == 1 && s_path[0] == '/') {      /* parent is the root: "/name" */
      for (int i = 0; i < nl; i++) s_path[1 + i] = s_fno.fname[i];
      s_path[1 + nl] = 0;
    } else {
      s_path[pl] = '/';
      for (int i = 0; i < nl; i++) s_path[pl + 1 + i] = s_fno.fname[i];
      s_path[pl + 1 + nl] = 0;
    }
    int childlen = (int)strlen(s_path);

    if (s_fno.fattrib & AM_DIR) {           /* descend */
      if (depth + 1 >= FS_RMTREE_MAX_DEPTH) { fr = FR_NOT_ENOUGH_CORE; break; }
      fr = f_opendir(&s_ds[depth + 1], s_path);
      if (fr != FR_OK) break;
      depth++;
      s_plen[depth] = childlen;
    } else {                                /* delete a file */
      if (s_fno.fattrib & AM_RDO) f_chmod(s_path, 0, AM_RDO);
      fr = f_unlink(s_path);
      if (fr != FR_OK) break;
      s_path[pl] = 0;                       /* restore parent path for next readdir */
    }
  }

  if (fr != FR_OK) {                        /* close any DIRs still open */
    for (int i = 0; i <= depth && i < FS_RMTREE_MAX_DEPTH; i++) f_closedir(&s_ds[i]);
  }
  return fr;
}

FRESULT fsop_delete(const char* path) {
  FILINFO fno;
  FRESULT fr = f_stat(path, &fno);
  if (fr != FR_OK) return fr;
  if (fno.fattrib & AM_DIR) return rmtree(path);
  if (fno.fattrib & AM_RDO) f_chmod(path, 0, AM_RDO);   /* clear RO so unlink works */
  return f_unlink(path);
}

/* ---- copy --------------------------------------------------------------- */

/* Big copy buffer + the dst-path stack live in EWRAM (.sbss, the same section
 * tonc's EWRAM_BSS uses) so they don't eat the 32 KiB IWRAM stack — declared
 * here without pulling in tonc/sys headers, keeping this file pure C. The copy
 * reuses the rmtree DIR stack (s_ds) and src path (s_path); the two ops never
 * run at the same time. */
#define FS_EWRAM __attribute__((section(".sbss")))
static uint8_t FS_EWRAM s_copybuf[4096] __attribute__((aligned(4)));
static char    FS_EWRAM s_dpath[FS_PATH_CAP];
static int              s_dplen[FS_RMTREE_MAX_DEPTH];

/* Copy a single file. Overwrites dst (FA_CREATE_ALWAYS); removes a partial dst
 * on any error so a failed copy never leaves a half-written file behind. */
static FRESULT copy_one_file(const char* src, const char* dst) {
  FIL fsrc, fdst;
  FRESULT fr = f_open(&fsrc, src, FA_READ);
  if (fr != FR_OK) return fr;
  fr = f_open(&fdst, dst, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK) { f_close(&fsrc); return fr; }

  for (;;) {
    UINT br = 0, bw = 0;
    fr = f_read(&fsrc, s_copybuf, sizeof(s_copybuf), &br);
    if (fr != FR_OK || br == 0) break;
    fr = f_write(&fdst, s_copybuf, br, &bw);
    if (fr != FR_OK) break;
    if (bw < br) { fr = FR_DENIED; break; }   /* short write = disk full */
  }

  FRESULT frc = f_close(&fdst);               /* a write-handle close can surface errors */
  f_close(&fsrc);
  if (fr == FR_OK) fr = frc;
  if (fr != FR_OK) f_unlink(dst);             /* drop the partial destination */
  return fr;
}

/* Recursively copy src_root -> dst_root. Mirrors rmtree's explicit-stack walk
 * but creates dst dirs and copies files. dst_root must NOT be inside src_root
 * (the caller guards this) or the walk would copy into its own growing tree. */
static FRESULT copy_tree(const char* src_root, const char* dst_root) {
  int sn = 0; for (; src_root[sn] && sn < FS_PATH_CAP - 1; sn++) s_path[sn] = src_root[sn];
  s_path[sn] = 0;
  int dn = 0; for (; dst_root[dn] && dn < FS_PATH_CAP - 1; dn++) s_dpath[dn] = dst_root[dn];
  s_dpath[dn] = 0;

  FRESULT fr = f_mkdir(s_dpath);
  if (fr != FR_OK && fr != FR_EXIST) return fr;

  int depth = 0;
  fr = f_opendir(&s_ds[0], s_path);
  if (fr != FR_OK) return fr;
  s_plen[0] = sn; s_dplen[0] = dn;

  while (depth >= 0) {
    fr = f_readdir(&s_ds[depth], &s_fno);
    if (fr != FR_OK) break;

    if (s_fno.fname[0] == 0) {                /* level done: pop */
      f_closedir(&s_ds[depth]);
      depth--;
      if (depth >= 0) { s_path[s_plen[depth]] = 0; s_dpath[s_dplen[depth]] = 0; }
      continue;
    }

    int pl = s_plen[depth], dl = s_dplen[depth], nl = (int)strlen(s_fno.fname);
    if (pl + 1 + nl + 1 > FS_PATH_CAP || dl + 1 + nl + 1 > FS_PATH_CAP) {
      fr = FR_NOT_ENOUGH_CORE; break;
    }
    /* append the child name to both src and dst paths (root has no extra '/') */
    if (pl == 1 && s_path[0] == '/') { for (int i = 0; i < nl; i++) s_path[1 + i] = s_fno.fname[i]; s_path[1 + nl] = 0; }
    else { s_path[pl] = '/'; for (int i = 0; i < nl; i++) s_path[pl + 1 + i] = s_fno.fname[i]; s_path[pl + 1 + nl] = 0; }
    if (dl == 1 && s_dpath[0] == '/') { for (int i = 0; i < nl; i++) s_dpath[1 + i] = s_fno.fname[i]; s_dpath[1 + nl] = 0; }
    else { s_dpath[dl] = '/'; for (int i = 0; i < nl; i++) s_dpath[dl + 1 + i] = s_fno.fname[i]; s_dpath[dl + 1 + nl] = 0; }
    int scl = (int)strlen(s_path), dcl = (int)strlen(s_dpath);

    if (s_fno.fattrib & AM_DIR) {             /* make dst dir + descend */
      fr = f_mkdir(s_dpath);
      if (fr != FR_OK && fr != FR_EXIST) break;
      if (depth + 1 >= FS_RMTREE_MAX_DEPTH) { fr = FR_NOT_ENOUGH_CORE; break; }
      fr = f_opendir(&s_ds[depth + 1], s_path);
      if (fr != FR_OK) break;
      depth++;
      s_plen[depth] = scl; s_dplen[depth] = dcl;
    } else {                                  /* copy a file */
      fr = copy_one_file(s_path, s_dpath);
      if (fr != FR_OK) break;
      s_path[pl] = 0; s_dpath[dl] = 0;        /* restore parent paths */
    }
  }

  if (fr != FR_OK) {
    for (int i = 0; i <= depth && i < FS_RMTREE_MAX_DEPTH; i++) f_closedir(&s_ds[i]);
  }
  return fr;
}

FRESULT fsop_copy(const char* src, const char* dst) {
  FILINFO fno;
  FRESULT fr = f_stat(src, &fno);
  if (fr != FR_OK) return fr;
  if (fno.fattrib & AM_DIR) return copy_tree(src, dst);
  return copy_one_file(src, dst);
}

/* ---- hex-editor verified write ----------------------------------------- */

static uint8_t FS_EWRAM s_cmpbuf[4096] __attribute__((aligned(4)));   /* verify pass */

/* Overwrite the bytes in [base, base+len) that fall under an edit. */
static void apply_edits_chunk(uint8_t* buf, uint64_t base, UINT len,
                              const HexEdit* edits, int n) {
  for (int i = 0; i < n; i++)
    if (edits[i].off >= base && edits[i].off < base + len)
      buf[edits[i].off - base] = edits[i].val;
}

FRESULT fsop_apply_edits(const char* path, const HexEdit* edits, int n) {
  char tmp[FS_PATH_CAP], bak[FS_PATH_CAP];
  if ((unsigned)strlen(path) + 9 >= FS_PATH_CAP) return FR_NOT_ENOUGH_CORE;
  strcpy(tmp, path); strcat(tmp, ".hexnew~");
  strcpy(bak, path); strcat(bak, ".bak~");

  /* 1. write tmp = original with edits applied */
  FIL fin, fout;
  FRESULT fr = f_open(&fin, path, FA_READ);
  if (fr != FR_OK) return fr;
  fr = f_open(&fout, tmp, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK) { f_close(&fin); return fr; }
  uint64_t pos = 0;
  for (;;) {
    UINT br = 0, bw = 0;
    fr = f_read(&fin, s_copybuf, sizeof(s_copybuf), &br);
    if (fr != FR_OK || br == 0) break;
    apply_edits_chunk(s_copybuf, pos, br, edits, n);
    fr = f_write(&fout, s_copybuf, br, &bw);
    if (fr != FR_OK) break;
    if (bw < br) { fr = FR_DENIED; break; }   /* disk full */
    pos += br;
  }
  FRESULT fc = f_close(&fout);
  f_close(&fin);
  if (fr == FR_OK) fr = fc;
  if (fr != FR_OK) { f_unlink(tmp); return fr; }

  /* 2. verify: tmp must equal original-with-edits, byte for byte */
  fr = f_open(&fin, path, FA_READ);
  if (fr != FR_OK) { f_unlink(tmp); return fr; }
  FIL fchk;
  fr = f_open(&fchk, tmp, FA_READ);
  if (fr != FR_OK) { f_close(&fin); f_unlink(tmp); return fr; }
  pos = 0;
  bool ok = true;
  for (;;) {
    UINT ba = 0, bb = 0;
    FRESULT ra = f_read(&fin,  s_copybuf, sizeof(s_copybuf), &ba);
    FRESULT rb = f_read(&fchk, s_cmpbuf,  sizeof(s_cmpbuf),  &bb);
    if (ra != FR_OK || rb != FR_OK || ba != bb) { ok = false; break; }
    if (ba == 0) break;                       /* clean EOF on both */
    apply_edits_chunk(s_copybuf, pos, ba, edits, n);
    if (memcmp(s_copybuf, s_cmpbuf, ba) != 0) { ok = false; break; }
    pos += ba;
  }
  f_close(&fin);
  f_close(&fchk);
  if (!ok) { f_unlink(tmp); return FR_INT_ERR; }   /* verify failed: original untouched */

  /* 3. swap: back up the original, then move the verified temp into place */
  FILINFO fno;
  if (f_stat(bak, &fno) == FR_OK) {                 /* replace a prior backup */
    if (fno.fattrib & AM_RDO) f_chmod(bak, 0, AM_RDO);
    f_unlink(bak);
  }
  fr = f_rename(path, bak);                          /* original survives as <path>.bak~ */
  if (fr != FR_OK) { f_unlink(tmp); return fr; }
  fr = f_rename(tmp, path);
  if (fr != FR_OK) {                                 /* restore on failure */
    if (f_rename(bak, path) == FR_OK) f_unlink(tmp); /* restored -> drop the stray temp */
    /* else: original survives as <path>.bak~ and the new bytes as <path>.hexnew~ */
    return fr;
  }
  return FR_OK;
}
