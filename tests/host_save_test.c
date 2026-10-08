/* Regression harness for fsop_save_buffer (the verified buffer save behind the text
 * editor, pins, shortcuts and button slots). Runs the SHIPPED source/fs_ops.c and
 * lib/fatfs/ff.c over a RAM disk (tests/hostfat, from PokeDNA) and asserts on the
 * resulting BYTES, not on return codes alone. wrap.h routes f_open/f_rename/f_unlink
 * through the wrappers at the bottom so a case can fail the Nth matching call or
 * corrupt the temp between write and read-back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "fs_ops.h"
#include "ramdisk.h"

static int passed, fails;
#define CHECK(c, ...) do { if (c) passed++; else { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define P   "/doc.txt"
#define TMP P ".txtnew~"
#define BAK P ".bak~"
#define CAP 40000u

static FATFS s_fs;
static BYTE  s_work[1024];
static unsigned char NEWB[CAP], OLDB[CAP], RB[CAP];

/* ---- fault injection (the wrappers) ------------------------------------- */
enum { K_OPEN, K_RENAME, K_UNLINK };
typedef struct { int kind; const char* a_suf; const char* b_suf; int skip; int armed; int hits; } Rule;
static Rule rules[4];
static int tamper_mode;   /* 0 none, 1 flip last, 2 flip first, 3 short by 1, 4 long by 1 */
static long tamper_len;

static int ends(const char* s, const char* suf) {
  size_t n, m;
  if (!suf) return 1;
  if (!s) return 0;
  n = strlen(s); m = strlen(suf);
  return n >= m && !strcmp(s + n - m, suf);
}
static void rules_clear(void) { memset(rules, 0, sizeof rules); tamper_mode = 0; }
static void rule_add(int kind, const char* a_suf, const char* b_suf, int skip) {
  int i;
  for (i = 0; i < 4; i++) if (!rules[i].armed) {
    rules[i].kind = kind; rules[i].a_suf = a_suf; rules[i].b_suf = b_suf;
    rules[i].skip = skip; rules[i].armed = 1; rules[i].hits = 0; return;
  }
}
/* 1 = fail this call (one-shot per rule, after `skip` matching calls) */
static int rule_hit(int kind, const char* a, const char* b) {
  int i;
  for (i = 0; i < 4; i++) {
    Rule* r = &rules[i];
    if (!r->armed || r->kind != kind || !ends(a, r->a_suf) || !ends(b, r->b_suf)) continue;
    if (r->skip > 0) { r->skip--; continue; }
    r->armed = 0; r->hits++; return 1;
  }
  return 0;
}
static void do_tamper(const char* p) {
  FIL f; UINT n; unsigned char c = 0;
  if (f_open(&f, p, FA_READ | FA_WRITE) != FR_OK) return;
  if (tamper_mode == 1 || tamper_mode == 2) {
    FSIZE_t at = (tamper_mode == 1) ? (FSIZE_t)(tamper_len - 1) : 0;
    f_lseek(&f, at); f_read(&f, &c, 1, &n); c ^= 0x55;
    f_lseek(&f, at); f_write(&f, &c, 1, &n);
  } else if (tamper_mode == 3) { f_lseek(&f, (FSIZE_t)(tamper_len - 1)); f_truncate(&f); }
  else if (tamper_mode == 4)   { c = 'x'; f_lseek(&f, (FSIZE_t)tamper_len); f_write(&f, &c, 1, &n); }
  f_close(&f);
}
FRESULT wrap_f_open(FIL* fp, const TCHAR* path, BYTE mode) {
  if (mode == FA_READ && tamper_mode && ends(path, ".txtnew~")) { do_tamper(path); tamper_mode = 0; }
  if (rule_hit(K_OPEN, path, NULL)) return FR_DISK_ERR;
  return f_open(fp, path, mode);
}
FRESULT wrap_f_rename(const TCHAR* a, const TCHAR* b) {
  if (rule_hit(K_RENAME, a, b)) return FR_DISK_ERR;
  return f_rename(a, b);
}
FRESULT wrap_f_unlink(const TCHAR* p) {
  if (rule_hit(K_UNLINK, p, NULL)) return FR_DISK_ERR;
  return f_unlink(p);
}
/* The wrappers above call the REAL functions: wrap.h's macros only reach fs_ops.c, not
 * this file, so f_open here is ff.c's own. */

/* ---- helpers ------------------------------------------------------------ */
static void fill(unsigned char* p, unsigned n, unsigned seed) {
  unsigned i;
  for (i = 0; i < n; i++) p[i] = (unsigned char)((i * 31u + seed * 7u + (i >> 9) * 13u) & 0xFF);
}
static void fresh_card(unsigned sectors) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(sectors);
  rules_clear();
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs");
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount");
}
static void remount(void) { f_mount(0, "", 0); CHECK(f_mount(&s_fs, "", 1) == FR_OK, "remount"); }
static int exists(const char* p) { FILINFO fi; return f_stat(p, &fi) == FR_OK; }
static long readf(const char* p, unsigned char* out) {   /* -1 = missing */
  FIL f; UINT br = 0; long n;
  if (f_open(&f, p, FA_READ) != FR_OK) return -1;
  n = (long)f_size(&f);
  if (n > (long)CAP) n = (long)CAP;
  if (n > 0 && (f_read(&f, out, (UINT)n, &br) != FR_OK || br != (UINT)n)) n = -2;
  f_close(&f);
  return n;
}
static int is(const char* p, const unsigned char* want, unsigned n) {
  long got = readf(p, RB);
  return got == (long)n && (n == 0 || !memcmp(RB, want, n));
}
static void put(const char* p, const unsigned char* d, unsigned n) {
  FIL f; UINT bw;
  CHECK(f_open(&f, p, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "put open %s", p);
  if (n) f_write(&f, d, n, &bw);
  f_close(&f);
}
static const unsigned SIZES[] = { 0, 1, 511, 512, 4095, 4096, 4097, 8192, 12289, 32768 };
#define NSIZES (sizeof SIZES / sizeof SIZES[0])

/* Point `victim`'s directory entry at `donor`'s first cluster: two names, one chain. */
static int crosslink(const char* victim, const char* donor) {
  FIL f; DWORD vc, dc, vsz; unsigned s, off, found = 0, nsec;
  if (f_open(&f, victim, FA_READ) != FR_OK) return 0;
  vc = f.obj.sclust; vsz = (DWORD)f_size(&f); f_close(&f);
  if (f_open(&f, donor, FA_READ) != FR_OK) return 0;
  dc = f.obj.sclust; f_close(&f);
  if (s_fs.fs_type > FS_FAT16) return 0;
  nsec = (s_fs.n_rootdir * 32u + 511u) / 512u;
  for (s = 0; s < nsec; s++) {
    unsigned char* b = rd_sector((unsigned)s_fs.dirbase + s);
    for (off = 0; b && off < 512; off += 32) {
      unsigned char* e = b + off;
      DWORD sz = e[28] | (e[29] << 8) | (e[30] << 16) | ((DWORD)e[31] << 24);
      if (e[0] == 0 || e[0] == 0xE5 || (e[11] & 0x3F) != 0x20) continue;
      if (e[26] == (vc & 255) && e[27] == ((vc >> 8) & 255) && sz == vsz) {
        e[26] = (unsigned char)(dc & 255); e[27] = (unsigned char)((dc >> 8) & 255); found++;
      }
    }
  }
  remount();
  return found == 1;
}

/* ---- cases -------------------------------------------------------------- */
static void t_sizes(void) {
  unsigned i;
  for (i = 0; i < NSIZES; i++) {
    unsigned n = SIZES[i]; FRESULT fr;
    fresh_card(2048);
    fill(NEWB, n, 1); fill(OLDB, n, 2);
    fr = fsop_save_buffer(P, NEWB, n, true);                       /* target did not exist */
    CHECK(fr == FR_OK, "new file n=%u fr=%d", n, (int)fr);
    CHECK(is(P, NEWB, n), "new file bytes n=%u", n);
    CHECK(!exists(TMP) && !exists(BAK), "new file leaves no temp/bak n=%u", n);
    fr = fsop_save_buffer(P, OLDB, n, true);                       /* overwrite, keep .bak~ */
    CHECK(fr == FR_OK, "overwrite n=%u fr=%d", n, (int)fr);
    CHECK(is(P, OLDB, n), "overwrite bytes n=%u", n);
    CHECK(is(BAK, NEWB, n), "bak holds previous bytes n=%u", n);
    CHECK(!exists(TMP), "no temp after overwrite n=%u", n);
    fr = fsop_save_buffer(P, NEWB, n, false);                      /* keep_backup=false */
    CHECK(fr == FR_OK && is(P, NEWB, n) && !exists(BAK) && !exists(TMP), "no-backup save n=%u", n);
  }
}

static void t_verify_fail(void) {
  static const int modes[] = { 1, 2, 3, 4 };
  static const char* const nm[] = { "flip-last", "flip-first", "short", "long" };
  unsigned i, m;
  for (m = 0; m < 4; m++) for (i = 0; i < NSIZES; i++) {
    unsigned n = SIZES[i]; FRESULT fr;
    if (n == 0 && modes[m] != 4) continue;                         /* nothing to flip/shorten */
    fresh_card(2048);
    fill(NEWB, n, 1); fill(OLDB, n + 3, 2);
    put(P, OLDB, n + 3);
    tamper_mode = modes[m]; tamper_len = n;
    fr = fsop_save_buffer(P, NEWB, n, true);
    CHECK(fr == FR_INT_ERR, "%s n=%u must fail verify, fr=%d", nm[m], n, (int)fr);
    CHECK(is(P, OLDB, n + 3), "%s n=%u original untouched", nm[m], n);
    CHECK(!exists(TMP) && !exists(BAK), "%s n=%u temp dropped, no bak", nm[m], n);
    rules_clear();
  }
}

static void t_leftover(void) {
  FRESULT fr;
  fresh_card(2048);
  fill(OLDB, 700, 2); fill(NEWB, 900, 1);
  put(P, OLDB, 700); put(TMP, (const unsigned char*)"junk", 4);
  fr = fsop_save_buffer(P, NEWB, 900, true);
  CHECK(fr == FSOP_ERR_LEFTOVER, "leftover temp refused, fr=%d", (int)fr);
  CHECK(is(TMP, (const unsigned char*)"junk", 4), "leftover temp not truncated");
  CHECK(is(P, OLDB, 700) && !exists(BAK), "leftover: original untouched, no bak");
  /* leftover with the target absent too */
  fresh_card(2048);
  put(TMP, (const unsigned char*)"junk", 4);
  fr = fsop_save_buffer(P, NEWB, 900, true);
  CHECK(fr == FSOP_ERR_LEFTOVER && is(TMP, (const unsigned char*)"junk", 4) && !exists(P), "leftover, no target");
}

static void t_crosslink(void) {
  FRESULT fr;
  fresh_card(2048);
  fill(OLDB, 3000, 2); fill(NEWB, 3000, 3);
  put(P, OLDB, 3000); put(BAK, NEWB, 3000);
  CHECK(fsop_same_chain(P, BAK) == 0, "distinct files: same_chain 0");
  CHECK(fsop_same_chain(P, "/nope") == -1 && fsop_same_chain("/nope", P) == -1, "missing file: same_chain -1");
  CHECK(crosslink(BAK, P), "crosslink set up");
  CHECK(fsop_same_chain(P, BAK) == 1, "cross-linked: same_chain 1");
  fill(NEWB, 3000, 4);
  fr = fsop_save_buffer(P, NEWB, 3000, true);
  CHECK(fr == FSOP_ERR_SHARED, "cross-linked bak refused, fr=%d", (int)fr);
  CHECK(is(P, OLDB, 3000), "cross-linked: target bytes intact");
  CHECK(is(BAK, OLDB, 3000), "cross-linked: bak not deleted");
  CHECK(!exists(TMP), "cross-linked: temp dropped");
  /* unknown (open of .bak~ fails) must be treated as shared, never deleted */
  fresh_card(2048);
  put(P, OLDB, 3000); put(BAK, NEWB, 3000);
  rule_add(K_OPEN, ".bak~", NULL, 0);
  fr = fsop_save_buffer(P, NEWB, 3000, true);
  CHECK(fr == FSOP_ERR_SHARED, "unknown chain treated as shared, fr=%d", (int)fr);
  CHECK(is(P, OLDB, 3000) && is(BAK, NEWB, 3000), "unknown chain: nothing deleted");
}

static void t_zero_pair(void) {
  FRESULT fr;
  fresh_card(2048);
  put(P, OLDB, 0); put(BAK, OLDB, 0);                              /* both sclust == 0 */
  CHECK(fsop_same_chain(P, BAK) == 0, "0-byte pair is not 'shared'");
  fr = fsop_save_buffer(P, (const unsigned char*)"hello", 5, true);
  CHECK(fr == FR_OK && is(P, (const unsigned char*)"hello", 5) && is(BAK, OLDB, 0) && !exists(TMP), "0-byte pair saved, fr=%d", (int)fr);
}

static void t_readonly_bak(void) {
  FRESULT fr;
  fresh_card(2048);
  fill(OLDB, 1500, 2); fill(NEWB, 1600, 1);
  put(P, OLDB, 1500); put(BAK, NEWB, 1600);
  CHECK(f_chmod(BAK, AM_RDO, AM_RDO) == FR_OK, "chmod bak ro");
  fr = fsop_save_buffer(P, NEWB, 1600, true);
  CHECK(fr == FR_OK && is(P, NEWB, 1600) && is(BAK, OLDB, 1500) && !exists(TMP), "read-only bak replaced, fr=%d", (int)fr);
}

static void t_rename_faults(void) {
  FRESULT fr;
  fill(OLDB, 2000, 2); fill(NEWB, 2500, 1);
  /* 1. path -> bak fails: nothing changed */
  fresh_card(2048); put(P, OLDB, 2000);
  rule_add(K_RENAME, NULL, ".bak~", 0);
  fr = fsop_save_buffer(P, NEWB, 2500, true);
  CHECK(fr != FR_OK && is(P, OLDB, 2000) && !exists(TMP) && !exists(BAK), "rename#1 fail: original in place, fr=%d", (int)fr);
  /* 2. tmp -> path fails, restore works: original back, temp dropped */
  fresh_card(2048); put(P, OLDB, 2000);
  rule_add(K_RENAME, ".txtnew~", NULL, 0);
  fr = fsop_save_buffer(P, NEWB, 2500, true);
  CHECK(fr != FR_OK && is(P, OLDB, 2000) && !exists(TMP) && !exists(BAK), "rename#2 fail: restored, fr=%d", (int)fr);
  /* 3. both fail: data survives only as .bak~ (old) + .txtnew~ (new), neither deleted */
  fresh_card(2048); put(P, OLDB, 2000);
  rule_add(K_RENAME, ".txtnew~", NULL, 0);
  rule_add(K_RENAME, ".bak~", NULL, 0);
  fr = fsop_save_buffer(P, NEWB, 2500, true);
  CHECK(fr != FR_OK && !exists(P), "double fail: target absent, fr=%d", (int)fr);
  CHECK(is(BAK, OLDB, 2000), "double fail: old bytes in .bak~");
  CHECK(is(TMP, NEWB, 2500), "double fail: new bytes kept in .txtnew~");
  /* 4. deleting the previous .bak~ fails: original and old bak intact */
  fresh_card(2048); put(P, OLDB, 2000); put(BAK, NEWB, 77);
  rule_add(K_UNLINK, ".bak~", NULL, 0);
  fr = fsop_save_buffer(P, NEWB, 2500, true);
  CHECK(fr != FR_OK && is(P, OLDB, 2000) && is(BAK, NEWB, 77) && !exists(TMP), "bak delete fail: nothing lost, fr=%d", (int)fr);
}

static void t_disk_full(void) {
  FRESULT fr; DWORD free0 = 0, free1 = 0; FATFS* fsp;
  fresh_card(256);                                                 /* ~128 KiB card */
  fill(OLDB, 40000, 2); fill(NEWB, 40000, 1);
  put(P, OLDB, 40000);
  f_getfree("", &free0, &fsp);
  fr = fsop_save_buffer(P, NEWB, 40000, true);                     /* temp alone needs 80 sectors; ~90 are free: fits */
  if (fr == FR_OK) {                                               /* fits: now fill the card so the next one cannot */
    unsigned i; FIL f; UINT bw; char nm[16];
    for (i = 0; i < 400; i++) {
      snprintf(nm, sizeof nm, "/f%u", i);
      if (f_open(&f, nm, FA_WRITE | FA_CREATE_NEW) != FR_OK) break;
      if (f_write(&f, OLDB, 512, &bw) != FR_OK || bw < 512) { f_close(&f); f_unlink(nm); break; }
      f_close(&f);
    }
    remount();
    f_getfree("", &free0, &fsp);
    CHECK(free0 < 80, "filler left the card nearly full (%lu free)", (unsigned long)free0);
    fill(OLDB, 40000, 5);
    fr = fsop_save_buffer(P, OLDB, 40000, true);
  }
  CHECK(fr != FR_OK, "disk full must fail, fr=%d", (int)fr);
  CHECK(is(P, NEWB, 40000), "disk full: original bytes intact");
  CHECK(!exists(TMP), "disk full: partial temp removed");
  f_getfree("", &free1, &fsp);
  CHECK(free1 >= free0, "disk full: clusters returned (%lu -> %lu)", (unsigned long)free0, (unsigned long)free1);
}

int main(void) {
  t_sizes(); t_verify_fail(); t_leftover(); t_crosslink(); t_zero_pair();
  t_readonly_bak(); t_rename_faults(); t_disk_full();
  printf("host_save_test: %d passed, %d failed\n", passed, fails);
  rd_free();
  return fails ? 1 : 0;
}
