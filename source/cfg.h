#ifndef CFG_H
#define CFG_H

#include <stdbool.h>

/* Persistent user settings for the SD file browser. Read on both carts; written
 * Omega-only (best-effort) to /file_browser_gba.cfg. Mirrors the record-mixer's
 * app_config INI pattern. */

enum { FREE_B = 0, FREE_KB, FREE_MB, FREE_GB, FREE_UNIT_COUNT };

typedef struct {
  int  theme;           /* 0..THEME_COUNT-1                       */
  int  sort_key;        /* FsSortKey 0=name 1=size 2=date         */
  bool sort_rev;        /* descending                             */
  bool viewer_hex;      /* default file viewer: hex (true)/text   */
  bool show_hidden;     /* list AM_HID entries                    */
  bool confirm_delete;  /* prompt before delete (default true)    */
  bool delete_to_trash; /* delete moves to /.sdtrash (default true) */
  int  trash_days;      /* auto-delete trashed items older than N days; 0 = off */
  int  jump;            /* LEFT/RIGHT jump distance in rows       */
  int  key_delay;       /* key_repeat_limits arg 1 (frames)       */
  int  key_speed;       /* key_repeat_limits arg 2 (frames)       */
  int  free_unit;       /* FREE_*                                 */
  char last_dir[256];   /* folder to reopen next launch           */
} Settings;

extern Settings g_set;

void cfg_defaults(void);            /* reset g_set to built-in defaults */
void cfg_load(const char* path);    /* read INI into g_set (defaults if file/key absent) */
bool cfg_save(const char* path);    /* write g_set (Omega-only; best-effort) */

#endif /* CFG_H */
