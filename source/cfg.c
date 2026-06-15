/*
 * INI settings for the SD file browser (/file_browser_gba.cfg). GBA-only (FatFs).
 * Large buffers live in EWRAM (.sbss) so they never sit on the 32 KiB IWRAM
 * stack. Generalized from the record-mixer's app_config.c (one key) to the
 * browser's full Settings struct. Reads work on both carts; writes are
 * EZ-Flash-Omega-only and best-effort (a failed save just means defaults next
 * launch).
 */
#include <stdio.h>   /* siprintf */
#include <string.h>

#include "ff.h"
#include "cfg.h"
#include "theme.h"   /* THEME_COUNT for clamping */

#define CFG_EWRAM __attribute__((section(".sbss")))

Settings g_set;

void cfg_defaults(void) {
  g_set.theme = 0;
  g_set.sort_key = 0;
  g_set.sort_rev = false;
  g_set.viewer_hex = true;
  g_set.show_hidden = false;
  g_set.confirm_delete = true;
  g_set.delete_to_trash = true;
  g_set.jump = 11;
  g_set.key_delay = 16;
  g_set.key_speed = 4;
  g_set.free_unit = FREE_MB;
  g_set.last_dir[0] = 0;
}

static int to_int(const char* v) {
  int n = 0, s = 1;
  if (*v == '-') { s = -1; v++; }
  while (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; }
  return n * s;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void cfg_load(const char* path) {
  cfg_defaults();
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) return;     /* no file: keep defaults */
  /* Our own file is ~400 B; 2 KiB leaves generous room for hand edits/comments.
   * A file larger than this is read-truncated (later keys lost) and just falls
   * back to defaults for those keys — acceptable for non-critical config. */
  static char CFG_EWRAM buf[2048];
  UINT br = 0;
  FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &br);
  f_close(&f);
  if (fr != FR_OK || br == 0) return;
  buf[br] = 0;

  char* p = buf;
  while (*p) {
    char* line = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    if (*p) { *p = 0; p++; }
    while (*p == '\n' || *p == '\r') p++;

    char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 0 || *s == '#' || *s == ';' || *s == '[') continue;

    char* eq = strchr(s, '=');
    if (!eq) continue;
    *eq = 0;
    char* key = s;
    char* val = eq + 1;
    int kl = (int)strlen(key);
    while (kl > 0 && (key[kl - 1] == ' ' || key[kl - 1] == '\t')) key[--kl] = 0;
    while (*val == ' ' || *val == '\t') val++;
    int vl = (int)strlen(val);
    while (vl > 0 && (val[vl - 1] == ' ' || val[vl - 1] == '\t')) val[--vl] = 0;

    if      (!strcmp(key, "theme"))          g_set.theme = clampi(to_int(val), 0, THEME_COUNT - 1);
    else if (!strcmp(key, "sort_key"))       g_set.sort_key = clampi(to_int(val), 0, 2);
    else if (!strcmp(key, "sort_rev"))       g_set.sort_rev = (val[0] == '1');
    else if (!strcmp(key, "viewer_hex"))     g_set.viewer_hex = (val[0] == '1');
    else if (!strcmp(key, "show_hidden"))    g_set.show_hidden = (val[0] == '1');
    else if (!strcmp(key, "confirm_delete")) g_set.confirm_delete = (val[0] == '1');
    else if (!strcmp(key, "delete_to_trash")) g_set.delete_to_trash = (val[0] == '1');
    else if (!strcmp(key, "jump"))           g_set.jump = clampi(to_int(val), 1, 99);
    else if (!strcmp(key, "key_delay"))      g_set.key_delay = clampi(to_int(val), 2, 60);
    else if (!strcmp(key, "key_speed"))      g_set.key_speed = clampi(to_int(val), 1, 30);
    else if (!strcmp(key, "free_unit"))      g_set.free_unit = clampi(to_int(val), 0, FREE_UNIT_COUNT - 1);
    else if (!strcmp(key, "last_dir")) {
      int i = 0;
      for (; val[i] && i < (int)sizeof(g_set.last_dir) - 1; i++) g_set.last_dir[i] = val[i];
      g_set.last_dir[i] = 0;
    }
  }
}

bool cfg_save(const char* path) {
  static char CFG_EWRAM out[768];
  int n = sniprintf(out, sizeof(out),    /* bounded: last_dir is up to 255 B */
    "[file_browser_gba]\n"
    "theme=%d\nsort_key=%d\nsort_rev=%d\nviewer_hex=%d\nshow_hidden=%d\n"
    "confirm_delete=%d\ndelete_to_trash=%d\njump=%d\nkey_delay=%d\nkey_speed=%d\n"
    "free_unit=%d\nlast_dir=%s\n",
    g_set.theme, g_set.sort_key, g_set.sort_rev ? 1 : 0, g_set.viewer_hex ? 1 : 0,
    g_set.show_hidden ? 1 : 0, g_set.confirm_delete ? 1 : 0,
    g_set.delete_to_trash ? 1 : 0, g_set.jump,
    g_set.key_delay, g_set.key_speed, g_set.free_unit, g_set.last_dir);
  if (n <= 0) return false;
  FIL f;
  if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
  UINT bw = 0;
  FRESULT fr = f_write(&f, out, (UINT)n, &bw);
  f_close(&f);
  return fr == FR_OK && bw == (UINT)n;
}
