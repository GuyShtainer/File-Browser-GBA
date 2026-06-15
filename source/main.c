/*
 * SD File Browser (File-Browser-GBA) — a GBA-native microSD file manager.
 *
 * Runs on BOTH EZ-Flash Omega DE and EverDrive GBA X5 (read works on both).
 * Browse/sort, inspect properties, view+hex-edit files; write ops (rename, copy/
 * cut/paste, duplicate, delete, mkdir, new file, attrs, swap-names, hex save) are
 * EZ-Flash-Omega-only and gated on can_write(). Settings + themes persist to
 * /sdbrowse.cfg (saved Omega-only, read on both).
 *
 * Browser keys:
 *   UP/DOWN  move (auto-repeat on hold)
 *   LEFT/RIGHT  jump by the configured distance (default 11 rows)
 *   L/R      page up / page down
 *   A        open folder / [..] up / open the actions menu on a file
 *   B        up one folder
 *   START    open the actions menu (file / folder / [..]) — View lives inside it
 *   SELECT   cycle the 6 sort states (Name/Size/Date x asc/desc)
 *   (selection mode: A mark · SELECT mark-all · START batch menu · B exit)
 *
 * Reuses the toolkit's shared layer: flashcartio (cart detect + sector I/O),
 * FatFs (lib/fatfs), the cartridge RTC (file timestamps), the triple logger
 * (source/log.*), and the Mode-3 bitmap UI (ui.c, vendored from the
 * record-mixer). Generic file ops live in fs_ops.c (pure C, host-testable).
 */

#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "flashcartio.h"
#include "ff.h"
#include "log.h"
#include "ui.h"
#include "fs_ops.h"
#include "osk.h"
#include "cfg.h"

#define LOG_PATH  "/sdbrowse_log.txt"
#define CFG_PATH  "/sdbrowse.cfg"
#define FS_MAX    256
#define PATH_MAX  256

/* Recycle bin. Deleted items are MOVED (same-volume f_rename — atomic, no copy)
 * into TRASH_DIR; a per-item sidecar "<stored><TRASH_SIDE>" records the original
 * full path so Restore can put it back. TRASH_DIR is hidden+system so it stays
 * out of normal browsing. ".origin~" is a reserved suffix (see is_reserved). */
#define TRASH_DIR   "/.sdtrash"
#define TRASH_SIDE  ".origin~"
static bool under_trash(const char* path);   /* true if path is /.sdtrash or inside it */

/* ---- layout (pixels) ---------------------------------------------------- */
#define HDR_Y         0
#define BOX_Y         10
#define BOX_H         100
#define ROW0_Y        13
#define ROW_H         8
#define LIST_ROWS     12
#define DETAIL_NAME_Y 112   /* selected entry: full-ish name              */
#define DETAIL_META_Y 122   /* selected entry: date/time + size           */
#define STATUS_Y      134
#define FOOT_Y        150

/* ---- browser state ------------------------------------------------------ */
static FsEntry  EWRAM_BSS g_entries[FS_MAX];
static int                g_n = 0;
static bool               g_trunc = false;
static char     EWRAM_BSS g_cwd[PATH_MAX];
static FsSortKey          g_sortkey = FS_SORT_NAME;
static bool               g_sortrev = false;
static uint64_t           g_free = 0, g_total = 0;
static bool               g_free_ok = false;

/* Clipboard for copy/cut/paste — a multi-item list captured from ONE source
 * directory; paste re-creates each item in the current directory. A single
 * Copy/Cut is just a 1-item clipboard. Names are packed NUL-separated. */
typedef enum { CLIP_NONE, CLIP_COPY, CLIP_CUT } ClipOp;
#define CLIP_BUF 4096
static ClipOp             g_clip_op = CLIP_NONE;
static char     EWRAM_BSS g_clip_dir[PATH_MAX];   /* source directory          */
static char     EWRAM_BSS g_clip_buf[CLIP_BUF];   /* packed NUL-separated names */
static int                g_clip_len = 0;         /* used bytes of g_clip_buf   */
static int                g_clip_count = 0;       /* number of names            */

/* Multi-select: marks align 1:1 with g_entries[] and clear on every rescan
 * (so they only ever apply to the directory currently shown). */
static bool     EWRAM_BSS g_marked[FS_MAX];
static bool               g_selmode = false;

/* Recursive keyword-search results (full paths + a dir flag), filled by the
 * Find action. g_find_sel holds the entry name to re-select after a find
 * navigates the browser into the match's folder ("" = none). */
#define FIND_MAX 128
static char     EWRAM_BSS g_find_paths[FIND_MAX][PATH_MAX];
static u8       EWRAM_BSS g_find_isdir[FIND_MAX];
static int                g_find_count = 0;
static bool               g_find_trunc = false;
static char     EWRAM_BSS g_find_sel[FS_NAME_CAP];
/* g_find_paths' row stride must equal fsop_find()'s out_paths[][FS_PATH_CAP]. */
_Static_assert(PATH_MAX == FS_PATH_CAP, "find path row stride mismatch");

/* ---- frame tick + input ------------------------------------------------- */

static void vsync(void) { VBlankIntrWait(); key_poll(); }

static u16 wait_keys(u16 mask) {
  u16 hit;
  do { vsync(); hit = key_hit(mask); } while (!hit);
  return hit;
}

static void show_msg(const char* title, const char* body) {
  ui_clear();
  ui_text(6, 70, UI_TITLE, title);
  if (body) ui_text(6, 84, UI_TEXT, body);
}

static void halt_msg(const char* msg) {
  log_line("HALT: %s", msg);
  log_flush_to_sd(LOG_PATH);
  ui_clear();
  ui_text(6, 60, UI_WARN, "HALT");
  ui_text(6, 76, UI_TEXT, msg);
  while (1) vsync();
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- path helpers ------------------------------------------------------- */

static bool at_root(void) { return g_cwd[0] == '/' && g_cwd[1] == 0; }

static const char* base_name(const char* path) {
  const char* p = strrchr(path, '/');
  return p ? p + 1 : path;
}

static bool path_join(const char* dir, const char* name, char* out) {
  unsigned dl = (unsigned)strlen(dir), nl = (unsigned)strlen(name);
  if (dl == 1 && dir[0] == '/') {
    if (1 + nl + 1 > PATH_MAX) return false;
    siprintf(out, "/%s", name);
  } else {
    if (dl + 1 + nl + 1 > PATH_MAX) return false;
    siprintf(out, "%s/%s", dir, name);
  }
  return true;
}

static void path_up(void) {
  int l = (int)strlen(g_cwd);
  if (l <= 1) return;
  int i = l - 1;
  while (i > 0 && g_cwd[i] != '/') i--;
  if (i == 0) g_cwd[1] = 0; else g_cwd[i] = 0;
}

/* Containing directory of `path` into `out` (root "/" if path is a top-level
 * entry). out must hold PATH_MAX bytes. */
static void parent_of(const char* path, char* out) {
  int l = 0; for (; path[l] && l < PATH_MAX - 1; l++) out[l] = path[l];
  out[l] = 0;
  int i = l - 1;
  while (i > 0 && out[i] != '/') i--;
  if (i <= 0) { out[0] = '/'; out[1] = 0; } else out[i] = 0;
}

/* ---- DOS date/time + formatting ---------------------------------------- */

static void dos_date(u32 d, int* yr, int* mo, int* dy) {
  u32 fd = d >> 16;
  *yr = (int)(((fd >> 9) & 0x7F) + 1980);
  *mo = (int)((fd >> 5) & 0xF);
  *dy = (int)(fd & 0x1F);
}
static void dos_time(u32 d, int* hh, int* mm) {
  u32 ft = d & 0xFFFF;
  *hh = (int)((ft >> 11) & 0x1F);
  *mm = (int)((ft >> 5) & 0x3F);
}

/* "YYYY-MM-DD HH:MM", or "(no date)" when the entry has no timestamp. */
static void fmt_datetime(u32 dosdt, char* out) {
  if (dosdt == 0) { strcpy(out, "(no date)"); return; }
  int yr, mo, dy, hh, mm;
  dos_date(dosdt, &yr, &mo, &dy);
  dos_time(dosdt, &hh, &mm);
  siprintf(out, "%04d-%02d-%02d %02d:%02d", yr, mo, dy, hh, mm);
}

/* Human-readable size without floating point (one decimal place). */
static void human_size(uint64_t b, char* out) {
  if (b < 1024ULL)
    siprintf(out, "%lu B", (unsigned long)b);
  else if (b < 1024ULL * 1024)
    siprintf(out, "%lu.%lu KB", (unsigned long)(b >> 10),
             (unsigned long)((b & 1023) * 10 / 1024));
  else if (b < 1024ULL * 1024 * 1024)
    siprintf(out, "%lu.%lu MB", (unsigned long)(b >> 20),
             (unsigned long)(((b >> 10) & 1023) * 10 / 1024));
  else
    siprintf(out, "%lu.%lu GB", (unsigned long)(b >> 30),
             (unsigned long)(((b >> 20) & 1023) * 10 / 1024));
}

static void attrib_str(uint8_t a, char* o) {
  o[0] = (a & AM_DIR) ? 'D' : '-';
  o[1] = (a & AM_RDO) ? 'R' : '-';
  o[2] = (a & AM_HID) ? 'H' : '-';
  o[3] = (a & AM_SYS) ? 'S' : '-';
  o[4] = (a & AM_ARC) ? 'A' : '-';
  o[5] = 0;
}

/* ---- listing model ------------------------------------------------------ */
/* Row 0 is a synthetic "[..]" up-entry whenever we are not at the root. */

static int br_rows(void) { return g_n + (at_root() ? 0 : 1); }

static FsEntry* br_entry(int row) {
  if (!at_root()) {
    if (row == 0) return NULL;        /* the [..] row */
    row -= 1;
  }
  if (row < 0 || row >= g_n) return NULL;
  return &g_entries[row];
}

static void rescan(void) {
  show_msg("Reading...", g_cwd);
  memset(g_marked, 0, sizeof(g_marked));    /* marks are per-directory */
  int r = fsop_list(g_cwd, g_entries, FS_MAX, &g_trunc, g_set.show_hidden);
  g_n = (r < 0) ? 0 : r;
  if (r < 0) log_line("opendir %s failed", g_cwd);
  /* The recycle bin (/.sdtrash) is an internal folder: never show it in the
   * normal browser (even with Show hidden ON) — it is reached only via the
   * Trash action — so the user can't accidentally file orphans into it. */
  if (g_n > 0 && at_root()) {
    const char* tname = base_name(TRASH_DIR);
    for (int i = 0; i < g_n; i++)
      if (!strcmp(g_entries[i].name, tname)) {
        for (int j = i; j < g_n - 1; j++) g_entries[j] = g_entries[j + 1];
        g_n--;
        break;
      }
  }
  /* remember the folder for next launch — but never the trash itself */
  if (r >= 0 && !under_trash(g_cwd)) strcpy(g_set.last_dir, g_cwd);
  fsop_sort(g_entries, g_n, g_sortkey, g_sortrev);
  uint64_t f = 0, t = 0;
  g_free_ok = (fsop_freespace(g_cwd, &f, &t) == FR_OK);
  g_free = f; g_total = t;
  log_line("scan %s: %d entries%s", g_cwd, g_n, g_trunc ? " (capped)" : "");
}

/* ---- rendering ---------------------------------------------------------- */

/* Spelled-out description of the active sort key + direction, shown on the
 * status bar. Folders always sort before files regardless of this. */
static const char* sort_label_of(int key, bool rev) {
  switch (key) {
    case FS_SORT_SIZE: return rev ? "Size big-small" : "Size small-big";
    case FS_SORT_DATE: return rev ? "Date new-old"   : "Date old-new";
    case FS_SORT_NAME:
    default:           return rev ? "Name Z-A"       : "Name A-Z";
  }
}
static const char* sort_label(void) { return sort_label_of((int)g_sortkey, g_sortrev); }

static const char* free_unit_name(int u) {
  switch (u) {
    case FREE_B:  return "Bytes";
    case FREE_KB: return "KB";
    case FREE_GB: return "GB";
    case FREE_MB:
    default:      return "MB";
  }
}

/* Free space formatted in the user's chosen unit (no decimals — status bar). */
static void fmt_free(uint64_t b, char* out) {
  switch (g_set.free_unit) {
    case FREE_B:  siprintf(out, "%luB",  (unsigned long)b);          break;
    case FREE_KB: siprintf(out, "%luKB", (unsigned long)(b >> 10));  break;
    case FREE_GB: siprintf(out, "%luGB", (unsigned long)(b >> 30));  break;
    case FREE_MB:
    default:      siprintf(out, "%luMB", (unsigned long)(b >> 20));  break;
  }
}

static void render_browser(int sel, int top) {
  ui_clear();
  int rows = br_rows();

  /* ui_truncate caps DISPLAY COLUMNS, not bytes; a UTF-8 name can be up to
   * 4 bytes/column, so these must hold max_cols*4+1 (plus the row suffix). */
  char line[128], nbuf[128], hdr[128];
  ui_truncate(hdr, g_cwd, 29);
  ui_text(2, HDR_Y, UI_TITLE, hdr);

  ui_panel(0, BOX_Y, 240, BOX_H, UI_PANEL, UI_BORDER);
  for (int r = 0; r < LIST_ROWS; r++) {
    int row = top + r;
    if (row >= rows) break;
    int y = ROW0_Y + r * ROW_H;
    FsEntry* e = br_entry(row);
    u16 ink;
    /* in selection mode, prefix each row with the mark state of its entry */
    const char* mk = "";
    if (g_selmode && e) mk = g_marked[(int)(e - g_entries)] ? "*" : " ";
    if (!e) {
      siprintf(line, "[..]  up one folder");
      ink = UI_WARN;
    } else if (e->is_dir) {
      ui_truncate(nbuf, e->name, g_selmode ? 18 : 20);
      siprintf(line, "%s%-18s    <DIR>", mk, nbuf);
      ink = UI_DIRCLR;
    } else {
      char szb[16];
      human_size(e->size, szb);
      ui_truncate(nbuf, e->name, g_selmode ? 14 : 16);
      siprintf(line, "%s%-15s %9s", mk, nbuf, szb);
      ink = UI_SAVECLR;
    }
    ui_text_sel(3, y, 234, row == sel, ink, line);
  }

  /* per-selection detail: a fuller name (29 cols vs the list's ~16) plus the
   * file's date/time and size — the full name lives in Properties (SELECT). */
  {
    FsEntry* se = br_entry(sel);
    char dn[128], dm[128], dt[20];
    if (!se) {
      ui_text(2, DETAIL_NAME_Y, UI_DIRCLR, "[..] parent folder");
    } else {
      ui_truncate(dn, se->name, 29);
      ui_text(2, DETAIL_NAME_Y, UI_SELTEXT, dn);
      fmt_datetime(se->dosdt, dt);
      if (se->is_dir) {
        siprintf(dm, "%s  <DIR>", dt);
      } else {
        char szb[16];
        human_size(se->size, szb);
        siprintf(dm, "%s  %s", dt, szb);
      }
      ui_text(2, DETAIL_META_Y, UI_DIM, dm);
    }
  }

  char st[64], stt[64];
  if (g_selmode) {                          /* selection mode: marked count + size */
    int mc = 0; uint64_t msz = 0;
    for (int i = 0; i < g_n; i++) if (g_marked[i]) { mc++; msz += g_entries[i].size; }
    char szb[16]; human_size(msz, szb);
    siprintf(st, "SEL %d marked  %s", mc, szb);
  } else {
    /* elaborate sort label + scroll position + free space (chosen unit) */
    if (g_free_ok) {
      char fb[16]; fmt_free(g_free, fb);
      siprintf(st, "%s %d/%d %s%s", sort_label(), rows ? sel + 1 : 0, rows,
               fb, g_trunc ? " +" : "");
    } else {
      siprintf(st, "%s %d/%d%s", sort_label(), rows ? sel + 1 : 0, rows,
               g_trunc ? " +" : "");
    }
  }
  ui_truncate(stt, st, 29);
  ui_text(2, STATUS_Y, UI_OK, stt);

  if (g_selmode) {
    ui_text(2, FOOT_Y, UI_OK, "A mark SE all ST batch B exit");
  } else if (g_clip_op != CLIP_NONE) {
    char fb[128], ft[128];
    if (g_clip_count == 1) {
      char nbf[64]; ui_truncate(nbf, g_clip_buf, 12);
      siprintf(fb, "[%s] %s  paste in menu", g_clip_op == CLIP_CUT ? "CUT" : "COPY", nbf);
    } else {
      siprintf(fb, "[%s] %d items  paste in menu", g_clip_op == CLIP_CUT ? "CUT" : "COPY", g_clip_count);
    }
    ui_truncate(ft, fb, 29);
    ui_text(2, FOOT_Y, UI_WARN, ft);
  } else if (rows == 0) {
    ui_text(2, FOOT_Y, UI_DIM, "Empty folder   ST = menu");
  } else {
    ui_text(2, FOOT_Y, UI_DIM, "A open B up ST menu SE:sort");
  }
}

static void properties_screen(const FsEntry* e) {
  ui_clear();
  ui_text(2, HDR_Y, UI_TITLE, "PROPERTIES");
  ui_panel(2, 12, 236, 120, UI_PANEL, UI_BORDER);

  int x = 8, y = 18;
  char line[160], szb[24], at[8], dt[20];

  /* full name, wrapped across up to 5 lines (~28 display cols each), advancing
   * a UTF-8-aware pointer so a multi-byte codepoint is never split. */
  ui_text(x, y, UI_DIM, "Name:"); y += 11;
  {
    const char* p = e->name;
    for (int ln = 0; ln < 5 && *p; ln++) {
      char seg[128];
      int cols = 0, b = 0;
      while (*p && cols < 28 && b < 120) {
        unsigned char c = (unsigned char)*p;
        int clen = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        for (int k = 0; k < clen && *p; k++) seg[b++] = *p++;
        cols++;
      }
      seg[b] = 0;
      ui_text(x + 6, y, UI_TEXT, seg); y += 10;
    }
    if (*p) { ui_text(x + 6, y, UI_DIM, "(name truncated)"); y += 10; }
  }
  y += 3;

  siprintf(line, "Type: %s", e->is_dir ? "Folder" : "File");
  ui_text(x, y, UI_TEXT, line); y += 12;

  if (!e->is_dir) {
    human_size(e->size, szb);
    if (e->size >> 32) siprintf(line, "Size: %s", szb);
    else siprintf(line, "Size: %s (%lu bytes)", szb, (unsigned long)e->size);
    ui_text(x, y, UI_TEXT, line); y += 12;
  }

  fmt_datetime(e->dosdt, dt);
  siprintf(line, "Modified: %s", dt);
  ui_text(x, y, UI_TEXT, line); y += 12;

  attrib_str(e->attrib, at);
  siprintf(line, "Attr: %s", at);  ui_text(x, y, UI_DIM, line);

  ui_text(2, FOOT_Y, UI_DIM, "B = back");
  wait_keys(KEY_B);
}

/* ---- write actions (Phase 1: Omega-only) ------------------------------- */

static bool can_write(void) { return active_flashcart == EZ_FLASH_OMEGA; }

static const char* fr_str(FRESULT fr) {
  switch (fr) {
    case FR_OK:              return "OK";
    case FR_EXIST:           return "already exists";
    case FR_DENIED:          return "denied (read-only?)";
    case FR_NO_FILE:
    case FR_NO_PATH:         return "not found";
    case FR_WRITE_PROTECTED: return "write protected";
    case FR_INVALID_NAME:    return "invalid name";
    case FR_NOT_ENOUGH_CORE: return "name too long / too deep";
    case FR_DISK_ERR:        return "SD error";
    case FR_TIMEOUT:         return "timeout";
    default:                 return "error";
  }
}

static bool confirm(const char* l1, const char* l2) {
  ui_clear();
  ui_text(6, 40, UI_WARN, "CONFIRM");
  if (l1) ui_text(6, 64, UI_TEXT, l1);
  if (l2) ui_text(6, 78, UI_WARN, l2);
  ui_text(6, 110, UI_DIM, "A = yes      B = no");
  return (wait_keys(KEY_A | KEY_B) & KEY_A) != 0;
}

static void msg_screen(const char* title, u16 ink, const char* body) {
  ui_clear();
  ui_text(6, 50, ink, title);
  if (body) ui_text(6, 72, UI_TEXT, body);
  ui_text(6, 110, UI_DIM, "B = back");
  wait_keys(KEY_B);
}

static bool do_mkdir(void) {
  char name[128], np[PATH_MAX];
  if (!osk_input("New folder name:", NULL, name, sizeof(name))) return false;
  if (!path_join(g_cwd, name, np)) { msg_screen("Path too long", UI_WARN, NULL); return false; }
  log_line("mkdir %s", np);
  FRESULT fr = fsop_mkdir(np);
  log_line("mkdir -> %d (%s)", fr, fr_str(fr));
  log_flush_to_sd(LOG_PATH);
  if (fr != FR_OK) { msg_screen("Create failed", UI_WARN, fr_str(fr)); return false; }
  return true;
}

static bool do_trash(const FsEntry* e);   /* recycle-bin move, defined below */

static bool do_delete(const FsEntry* e) {
  if (g_set.delete_to_trash) return do_trash(e);   /* recycle bin: move, don't erase */
  /* nb/l1 hold a UTF-8 name truncated to N cols -> up to ~4N bytes; size for the worst case. */
  char np[PATH_MAX], l1[128], nb[128];
  if (!path_join(g_cwd, e->name, np)) { msg_screen("Path too long", UI_WARN, NULL); return false; }
  if (g_set.confirm_delete) {
    ui_truncate(nb, e->name, 22);
    siprintf(l1, "Delete %s ?", nb);
    if (!confirm(l1, e->is_dir ? "FOLDER + ALL its contents!" : NULL)) return false;
  }
  show_msg("Deleting...", e->name);
  log_line("delete %s (dir=%d)", np, e->is_dir);
  FRESULT fr = fsop_delete(np);
  log_line("delete -> %d (%s)", fr, fr_str(fr));
  log_flush_to_sd(LOG_PATH);
  if (fr != FR_OK) { msg_screen("Delete failed", UI_WARN, fr_str(fr)); return false; }
  return true;
}

static bool do_rename(const FsEntry* e) {
  char newname[128], oldp[PATH_MAX], newp[PATH_MAX];
  if (!osk_input("Rename to:", e->name, newname, sizeof(newname))) return false;
  if (!strcmp(newname, e->name)) return false;            /* unchanged */
  if (!path_join(g_cwd, e->name, oldp) ||
      !path_join(g_cwd, newname, newp)) { msg_screen("Path too long", UI_WARN, NULL); return false; }
  log_line("rename %s -> %s", oldp, newp);
  FRESULT fr = fsop_rename(oldp, newp);                    /* FR_EXIST if target exists */
  log_line("rename -> %d (%s)", fr, fr_str(fr));
  log_flush_to_sd(LOG_PATH);
  if (fr != FR_OK) { msg_screen("Rename failed", UI_WARN, fr_str(fr)); return false; }
  return true;
}

/* Pick a non-colliding sidecar path (dst + ".old~[N]") to hold the existing
 * item during a safe overwrite. Returns false if no free name fits PATH_MAX. */
/* Reserved temp marker for the overwrite safe-swap. Paste refuses any item
 * whose name contains it, so a sidecar can never collide with a batch item. */
#define SIDECAR_MARK ".sdbtmp~"
static bool is_reserved(const char* name) {
  return strstr(name, SIDECAR_MARK) != NULL || strstr(name, TRASH_SIDE) != NULL;
}

static bool sidecar_name(const char* path, char* out) {
  if ((unsigned)strlen(path) + 12 >= PATH_MAX) return false;  /* room for SIDECAR_MARK + NN */
  FILINFO fno;
  for (int i = 0; i < 20; i++) {
    if (i == 0) siprintf(out, "%s" SIDECAR_MARK, path);
    else        siprintf(out, "%s" SIDECAR_MARK "%d", path, i);
    if (f_stat(out, &fno) != FR_OK) return true;              /* this name is free */
  }
  return false;
}

static void clip_reset(ClipOp op) {
  g_clip_op = op; g_clip_count = 0; g_clip_len = 0; g_clip_buf[0] = 0;
  strcpy(g_clip_dir, g_cwd);
}
static bool clip_add(const char* name) {
  int nl = (int)strlen(name);
  if (g_clip_len + nl + 1 > CLIP_BUF) return false;   /* clipboard full */
  strcpy(g_clip_buf + g_clip_len, name);
  g_clip_len += nl + 1;
  g_clip_count++;
  return true;
}
static void clip_clear(void) {
  g_clip_op = CLIP_NONE; g_clip_count = 0; g_clip_len = 0; g_clip_buf[0] = 0;
}

/* Paste one item src -> dst with the rule-#3 safe swap on overwrite: move any
 * existing dst aside, paste, then drop the sidecar on success or restore it on
 * failure — never leaving the user with neither the old nor a complete new. */
static FRESULT paste_one(const char* src, const char* dst, bool overwrite) {
  FILINFO fno;
  bool existed = (f_stat(dst, &fno) == FR_OK);
  if (existed && !overwrite) return FR_EXIST;
  if (existed) {
    char bak[PATH_MAX];
    if (!sidecar_name(dst, bak)) return FR_NOT_ENOUGH_CORE;
    FRESULT br = fsop_rename(dst, bak);
    if (br != FR_OK) return br;
    FRESULT fr = (g_clip_op == CLIP_CUT) ? fsop_rename(src, dst) : fsop_copy(src, dst);
    if (fr != FR_OK) {                       /* roll back: drop partial, restore original */
      FILINFO t; if (f_stat(dst, &t) == FR_OK) fsop_delete(dst);
      if (fsop_rename(bak, dst) != FR_OK)    /* restore failed: original survives in the sidecar */
        log_line("RECOVER: original kept at %s", bak);
      return fr;
    }
    fsop_delete(bak);
    return FR_OK;
  }
  return (g_clip_op == CLIP_CUT) ? fsop_rename(src, dst) : fsop_copy(src, dst);
}

static void do_copy(const FsEntry* e) { clip_reset(CLIP_COPY); clip_add(e->name); }
static void do_cut(const FsEntry* e)  { clip_reset(CLIP_CUT);  clip_add(e->name); }

/* True if dst is the same as, or nested inside, src (pasting a folder into
 * itself). The dst[sl]=='/' guard avoids /a matching /ab. */
static bool into_itself(const char* src, const char* dst) {
  unsigned sl = (unsigned)strlen(src);
  if (!strcmp(src, dst)) return true;
  return (!strncmp(dst, src, sl) && dst[sl] == '/');
}

static bool do_paste(void) {
  if (g_clip_op == CLIP_NONE || g_clip_count == 0) return false;
  if (under_trash(g_cwd)) {                    /* never paste into the recycle bin */
    msg_screen("Reserved folder", UI_WARN, "Cannot paste into the Trash");
    return false;
  }
  if (!strcmp(g_clip_dir, g_cwd)) {           /* pasting onto the items themselves */
    msg_screen("Already here", UI_WARN, "Paste into a different folder");
    return false;
  }

  /* pass 1: count collisions (skip reserved names + items that would self-paste) */
  int collisions = 0;
  for (int i = 0, off = 0; i < g_clip_count; i++) {
    const char* nm = g_clip_buf + off; off += (int)strlen(nm) + 1;
    if (is_reserved(nm)) continue;
    char src[PATH_MAX], dst[PATH_MAX];
    if (!path_join(g_clip_dir, nm, src) || !path_join(g_cwd, nm, dst)) continue;
    if (into_itself(src, dst)) continue;
    FILINFO fno; if (f_stat(dst, &fno) == FR_OK) collisions++;
  }

  bool overwrite = false;
  if (collisions > 0) {                       /* one confirm for the whole batch */
    char l1[48];
    siprintf(l1, "%d of %d already exist", collisions, g_clip_count);
    if (!confirm(l1, "Overwrite them?")) return false;
    overwrite = true;
  }

  show_msg(g_clip_op == CLIP_CUT ? "Moving..." : "Copying...",
           g_clip_count == 1 ? g_clip_buf : "items");
  int ok = 0, fail = 0, skip = 0;
  for (int i = 0, off = 0; i < g_clip_count; i++) {
    const char* nm = g_clip_buf + off; off += (int)strlen(nm) + 1;
    if (is_reserved(nm)) { skip++; continue; }   /* reserved sidecar namespace */
    char src[PATH_MAX], dst[PATH_MAX];
    if (!path_join(g_clip_dir, nm, src) || !path_join(g_cwd, nm, dst)) { fail++; continue; }
    if (into_itself(src, dst)) { skip++; continue; }
    FRESULT fr = paste_one(src, dst, overwrite);
    log_line("paste %s %s -> %s : %d", g_clip_op == CLIP_CUT ? "cut" : "copy", src, dst, fr);
    if (fr == FR_OK) ok++; else if (fr == FR_EXIST) skip++; else fail++;
  }
  log_flush_to_sd(LOG_PATH);

  if (g_clip_op == CLIP_CUT) clip_clear();   /* source consumed */
  if (fail || skip) {
    char m[48];
    siprintf(m, "%d ok  %d fail  %d skip", ok, fail, skip);
    msg_screen("Paste finished", UI_WARN, m);
  }
  return ok > 0;
}

static bool do_chmod_toggle(const FsEntry* e, u8 mask) {
  char np[PATH_MAX];
  if (!path_join(g_cwd, e->name, np)) return false;
  u8 set = (e->attrib & mask) ? 0 : mask;   /* toggle the bit */
  FRESULT fr = fsop_chmod(np, set, mask);
  log_line("chmod %s set=%02x mask=%02x -> %d", np, set, mask, fr);
  log_flush_to_sd(LOG_PATH);
  if (fr != FR_OK) { msg_screen("Attr change failed", UI_WARN, fr_str(fr)); return false; }
  return true;
}

/* Create a new empty file in the current folder (Omega-only). FA_CREATE_NEW
 * fails FR_EXIST rather than clobbering an existing file. */
static bool do_newfile(void) {
  char name[128], np[PATH_MAX];
  if (!osk_input("New file name:", NULL, name, sizeof(name))) return false;
  if (!path_join(g_cwd, name, np)) { msg_screen("Path too long", UI_WARN, NULL); return false; }
  FIL f;
  FRESULT fr = f_open(&f, np, FA_CREATE_NEW | FA_WRITE);
  if (fr == FR_OK) fr = f_close(&f);
  log_line("newfile %s -> %d (%s)", np, fr, fr_str(fr));
  log_flush_to_sd(LOG_PATH);
  if (fr != FR_OK) { msg_screen("Create failed", UI_WARN, fr_str(fr)); return false; }
  return true;
}

/* Build a non-colliding sibling name "<base> (copy[ N]).<ext>" for `name` in
 * `dir`, written as a full path into `out`. For a folder or an extension-less
 * file the suffix is appended. Returns false if no free name fits PATH_MAX. */
static bool duplicate_path(const char* dir, const char* name, bool is_dir, char* out) {
  const char* dot = is_dir ? NULL : strrchr(name, '.');   /* folders have no extension */
  int blen = (dot && dot != name) ? (int)(dot - name) : (int)strlen(name);
  const char* ext = (dot && dot != name) ? dot : "";     /* includes the '.' */
  char base[FS_NAME_CAP];
  if (blen > (int)sizeof(base) - 1) blen = (int)sizeof(base) - 1;
  memcpy(base, name, (size_t)blen); base[blen] = 0;

  char cand[FS_NAME_CAP];
  FILINFO fno;
  for (int i = 1; i <= 99; i++) {
    if (i == 1) sniprintf(cand, sizeof(cand), "%s (copy)%s", base, ext);
    else        sniprintf(cand, sizeof(cand), "%s (copy %d)%s", base, i, ext);
    if (!path_join(dir, cand, out)) return false;          /* too long */
    if (f_stat(out, &fno) != FR_OK) return true;           /* this name is free */
  }
  return false;
}

/* Duplicate the selected file/folder in place under an auto-suffixed sibling
 * name (Omega-only). Folders duplicate recursively via fsop_copy. */
static bool do_duplicate(const FsEntry* e) {
  char src[PATH_MAX], dst[PATH_MAX];
  if (!path_join(g_cwd, e->name, src)) { msg_screen("Path too long", UI_WARN, NULL); return false; }
  if (!duplicate_path(g_cwd, e->name, e->is_dir, dst)) { msg_screen("Cannot name copy", UI_WARN, "too long / too many"); return false; }
  show_msg("Duplicating...", e->name);
  log_line("duplicate %s -> %s", src, dst);
  FRESULT fr = fsop_copy(src, dst);
  log_line("duplicate -> %d (%s)", fr, fr_str(fr));
  log_flush_to_sd(LOG_PATH);
  if (fr != FR_OK) { msg_screen("Duplicate failed", UI_WARN, fr_str(fr)); return false; }
  return true;
}

/* ---- recycle bin (Trash) — Omega-only ----------------------------------- */
/*
 * Delete moves an item into TRASH_DIR instead of erasing it; Restore moves it
 * back to where it came from. The move is a same-volume f_rename (atomic, no
 * data copy — a trashed folder is just re-linked), so trashing/restoring even a
 * huge tree is instant and never duplicates bytes. Each trashed item "stored"
 * gets a sidecar file "<stored>.origin~" inside TRASH_DIR holding its original
 * full path, written BEFORE the move so a mid-op failure leaves at most a
 * harmless orphan sidecar (never a trashed item we can't trace).
 */

static int EWRAM_BSS g_trash_map[FS_MAX];   /* visible trash rows -> g_entries idx */

/* True if `path` is TRASH_DIR itself or sits inside it. */
static bool under_trash(const char* path) {
  static const char td[] = TRASH_DIR;
  unsigned tl = sizeof(td) - 1;
  if (strncmp(path, td, tl) != 0) return false;
  return path[tl] == 0 || path[tl] == '/';
}

/* Create TRASH_DIR if absent and mark it hidden+system (best-effort). */
static bool ensure_trash_dir(void) {
  FILINFO fno;
  if (f_stat(TRASH_DIR, &fno) == FR_OK) return (fno.fattrib & AM_DIR) != 0;
  FRESULT fr = fsop_mkdir(TRASH_DIR);
  if (fr != FR_OK && fr != FR_EXIST) return false;
  fsop_chmod(TRASH_DIR, AM_HID | AM_SYS, AM_HID | AM_SYS);   /* keep it out of normal listings */
  return true;
}

/* "<TRASH_DIR>/<stored><TRASH_SIDE>" into out (PATH_MAX). */
static bool sidecar_path(const char* stored, char* out) {
  if (!path_join(TRASH_DIR, stored, out)) return false;
  if (strlen(out) + strlen(TRASH_SIDE) + 1 > PATH_MAX) return false;
  strcat(out, TRASH_SIDE);
  return true;
}

/* True if `name` is an origin sidecar (ends with TRASH_SIDE). */
static bool is_origin_sidecar(const char* name) {
  int nl = (int)strlen(name), sl = (int)strlen(TRASH_SIDE);
  return nl >= sl && !strcmp(name + nl - sl, TRASH_SIDE);
}

/* Pick a stored name in TRASH_DIR that is free AND whose sidecar is free, so
 * two same-named items from different folders can both be trashed. */
static bool trash_free_name(const char* base, char* out) {
  char p[PATH_MAX], sp[PATH_MAX];
  FILINFO fno;
  for (int i = 1; i <= 999; i++) {
    if (i == 1) sniprintf(out, FS_NAME_CAP, "%s", base);
    else        sniprintf(out, FS_NAME_CAP, "%s (%d)", base, i);
    if (!path_join(TRASH_DIR, out, p)) return false;
    if (f_stat(p, &fno) == FR_OK) continue;            /* stored name taken */
    if (!sidecar_path(out, sp)) return false;
    if (f_stat(sp, &fno) == FR_OK) continue;           /* sidecar name taken */
    return true;
  }
  return false;
}

/* Write the original path of a trashed item into its sidecar. */
static FRESULT write_origin(const char* stored, const char* origpath) {
  char sp[PATH_MAX];
  if (!sidecar_path(stored, sp)) return FR_NOT_ENOUGH_CORE;
  FIL f;
  FRESULT fr = f_open(&f, sp, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK) return fr;
  UINT bw = 0; int len = (int)strlen(origpath);
  fr = f_write(&f, origpath, (UINT)len, &bw);
  FRESULT fc = f_close(&f);
  if (fr == FR_OK) fr = fc;
  if (fr == FR_OK && bw != (UINT)len) fr = FR_DENIED;
  if (fr != FR_OK) f_unlink(sp);
  return fr;
}

/* Read a trashed item's original path from its sidecar (NUL-terminated, CR/LF
 * stripped). Returns true only on a plausible absolute path. */
static bool read_origin(const char* stored, char* out) {
  char sp[PATH_MAX];
  if (!sidecar_path(stored, sp)) return false;
  FIL f;
  if (f_open(&f, sp, FA_READ) != FR_OK) return false;
  UINT br = 0;
  FRESULT fr = f_read(&f, out, PATH_MAX - 1, &br);
  f_close(&f);
  if (fr != FR_OK) return false;
  out[br] = 0;
  int n = (int)strlen(out);
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
  return n > 0 && out[0] == '/';
}

/* Move `src` into the trash, recording its origin. */
static FRESULT do_trash_path(const char* src) {
  if (under_trash(src)) return FR_INVALID_NAME;      /* don't re-trash the trash */
  if (is_reserved(base_name(src)))                   /* a name in our reserved temp/origin namespace */
    return FR_INVALID_NAME;                          /* would hide in the trash view — refuse (mirrors paste) */
  if (!ensure_trash_dir()) return FR_DENIED;
  char stored[FS_NAME_CAP], dst[PATH_MAX], sp[PATH_MAX];
  if (!trash_free_name(base_name(src), stored)) return FR_NOT_ENOUGH_CORE;
  FRESULT fr = write_origin(stored, src);            /* origin first */
  if (fr != FR_OK) return fr;
  if (!path_join(TRASH_DIR, stored, dst)) {
    if (sidecar_path(stored, sp)) f_unlink(sp);
    return FR_NOT_ENOUGH_CORE;
  }
  fr = fsop_rename(src, dst);
  if (fr == FR_DENIED) {                              /* clear read-only and retry once */
    f_chmod(src, 0, AM_RDO);
    fr = fsop_rename(src, dst);
  }
  if (fr != FR_OK) { if (sidecar_path(stored, sp)) f_unlink(sp); return fr; }   /* undo sidecar */
  return FR_OK;
}

/* Confirm + move the selected entry to the trash (the do_delete default). */
static bool do_trash(const FsEntry* e) {
  char np[PATH_MAX], l1[128], nb[128];
  if (!path_join(g_cwd, e->name, np)) { msg_screen("Path too long", UI_WARN, NULL); return false; }
  if (g_set.confirm_delete) {
    ui_truncate(nb, e->name, 20);
    siprintf(l1, "Move %s to Trash?", nb);
    if (!confirm(l1, "Restore it later from Trash")) return false;
  }
  show_msg("Moving to Trash...", e->name);
  FRESULT fr = do_trash_path(np);
  log_line("trash %s -> %d (%s)", np, fr, fr_str(fr));
  log_flush_to_sd(LOG_PATH);
  if (fr == FR_INVALID_NAME) { msg_screen("Can't trash this", UI_WARN, "reserved/system item - use Delete (perm)"); return false; }
  if (fr != FR_OK) { msg_screen("Trash failed", UI_WARN, fr_str(fr)); return false; }
  return true;
}

/* Where to restore a trashed item: its origin path if its parent still exists,
 * else the volume root; auto-suffixed "(restored[ N])" so an existing file at
 * the destination is never overwritten. *to_root flags the root fallback. */
static bool restore_dest(const char* origpath, char* out, bool* to_root) {
  char parent[PATH_MAX];
  parent_of(origpath, parent);
  FILINFO fno;
  bool parent_ok = (parent[0] == '/' && parent[1] == 0) ||
                   (f_stat(parent, &fno) == FR_OK && (fno.fattrib & AM_DIR));
  *to_root = !parent_ok;
  const char* dir = parent_ok ? parent : "/";
  const char* base = base_name(origpath);
  const char* dot = strrchr(base, '.');
  int blen = (dot && dot != base) ? (int)(dot - base) : (int)strlen(base);
  const char* ext = (dot && dot != base) ? dot : "";
  char nm[FS_NAME_CAP], cand[FS_NAME_CAP];
  if (blen > (int)sizeof(nm) - 1) blen = (int)sizeof(nm) - 1;
  memcpy(nm, base, (size_t)blen); nm[blen] = 0;
  for (int i = 0; i <= 99; i++) {
    if (i == 0)      sniprintf(cand, sizeof(cand), "%s%s", nm, ext);
    else if (i == 1) sniprintf(cand, sizeof(cand), "%s (restored)%s", nm, ext);
    else             sniprintf(cand, sizeof(cand), "%s (restored %d)%s", nm, i, ext);
    if (!path_join(dir, cand, out)) return false;
    if (f_stat(out, &fno) != FR_OK) return true;       /* free */
  }
  return false;
}

/* Restore the trashed item `stored` to its origin (or root). */
static bool do_restore(const char* stored) {
  char origin[PATH_MAX], src[PATH_MAX], dst[PATH_MAX], sp[PATH_MAX];
  if (!read_origin(stored, origin)) { msg_screen("No origin info", UI_WARN, "Move it out manually"); return false; }
  if (!path_join(TRASH_DIR, stored, src)) { msg_screen("Path too long", UI_WARN, NULL); return false; }
  bool to_root = false;
  if (!restore_dest(origin, dst, &to_root)) { msg_screen("Cannot place", UI_WARN, "name too long"); return false; }
  show_msg("Restoring...", stored);
  FRESULT fr = fsop_rename(src, dst);
  log_line("restore %s -> %s : %d (%s)", src, dst, fr, fr_str(fr));
  if (fr != FR_OK) { log_flush_to_sd(LOG_PATH); msg_screen("Restore failed", UI_WARN, fr_str(fr)); return false; }
  if (sidecar_path(stored, sp)) f_unlink(sp);          /* origin no longer needed */
  log_flush_to_sd(LOG_PATH);
  char nb[128]; ui_truncate(nb, dst, 28);
  msg_screen(to_root ? "Restored to root" : "Restored", UI_OK, nb);
  return true;
}

/* Permanently delete one trashed item + its sidecar (no further undo). */
static bool do_purge(const char* stored) {
  char p[PATH_MAX], sp[PATH_MAX];
  if (!path_join(TRASH_DIR, stored, p)) return false;
  FRESULT fr = fsop_delete(p);
  if (sidecar_path(stored, sp)) f_unlink(sp);
  log_line("purge %s -> %d (%s)", p, fr, fr_str(fr));
  log_flush_to_sd(LOG_PATH);
  if (fr != FR_OK && fr != FR_NO_FILE && fr != FR_NO_PATH) {
    msg_screen("Delete failed", UI_WARN, fr_str(fr)); return false;
  }
  return true;
}

/* Empty the whole trash (recursive). */
static bool trash_empty(void) {
  FILINFO fno;
  if (f_stat(TRASH_DIR, &fno) != FR_OK) return true;   /* nothing there */
  FRESULT fr = fsop_delete(TRASH_DIR);
  log_line("empty trash -> %d (%s)", fr, fr_str(fr));
  log_flush_to_sd(LOG_PATH);
  if (fr != FR_OK && fr != FR_NO_FILE && fr != FR_NO_PATH) {
    msg_screen("Empty failed", UI_WARN, fr_str(fr)); return false;
  }
  return true;
}

/* Build g_trash_map[] = the g_entries rows that are real trashed items (skip the
 * .origin~ sidecars). Returns the visible count. */
static int trash_visible(void) {
  int v = 0;
  for (int i = 0; i < g_n; i++)
    if (!is_origin_sidecar(g_entries[i].name)) g_trash_map[v++] = i;
  return v;
}

/* Re-list TRASH_DIR into g_entries (newest first). Returns the visible count. */
static int trash_scan(void) {
  int r = fsop_list(TRASH_DIR, g_entries, FS_MAX, &g_trunc, true);  /* show hidden trashed items too */
  g_n = (r < 0) ? 0 : r;
  fsop_sort(g_entries, g_n, FS_SORT_DATE, true);
  return trash_visible();
}

/* The Trash viewer: browse trashed items, Restore or permanently Delete each,
 * or Empty the whole bin. Clobbers g_entries (the browser is re-listed by the
 * caller on return). Returns true so the browser always refreshes. */
static bool trash_modal(void) {
  int vis = trash_scan();
  if (vis == 0) { msg_screen("Trash is empty", UI_DIM, "Deleted items appear here"); return true; }

  const int ROWS = LIST_ROWS;
  int sel = 0, top = 0;
  bool dirty = true;
  while (1) {
    if (sel >= vis) sel = vis - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    if (top < 0) top = 0;

    if (dirty) {
      ui_clear();
      char hdr[48];
      siprintf(hdr, "TRASH  (%d item%s)", vis, vis == 1 ? "" : "s");
      ui_text(2, HDR_Y, UI_TITLE, hdr);
      ui_panel(0, BOX_Y, 240, BOX_H, UI_PANEL, UI_BORDER);
      for (int rr = 0; rr < ROWS; rr++) {
        int row = top + rr;
        if (row >= vis) break;
        FsEntry* e = &g_entries[g_trash_map[row]];
        int y = ROW0_Y + rr * ROW_H;
        char line[128], nbuf[128];
        if (e->is_dir) {
          ui_truncate(nbuf, e->name, 20);
          siprintf(line, "%-20s    <DIR>", nbuf);
        } else {
          char szb[16]; human_size(e->size, szb);
          ui_truncate(nbuf, e->name, 16);
          siprintf(line, "%-15s %9s", nbuf, szb);
        }
        ui_text_sel(3, y, 234, row == sel, e->is_dir ? UI_DIRCLR : UI_SAVECLR, line);
      }
      /* detail: where the highlighted item came from */
      FsEntry* se = &g_entries[g_trash_map[sel]];
      char origin[PATH_MAX], db[128];
      if (read_origin(se->name, origin)) {
        ui_truncate(db, origin, 29);
        ui_text(2, DETAIL_NAME_Y, UI_SELTEXT, db);
        ui_text(2, DETAIL_META_Y, UI_DIM, "original location");
      } else {
        ui_text(2, DETAIL_NAME_Y, UI_DIM, "(origin unknown)");
      }
      ui_text(2, STATUS_Y, UI_DIM, "A: restore/delete   SELECT: empty");
      ui_text(2, FOOT_Y, UI_DIM, "B = back");
      dirty = false;
    }

    vsync();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_A | KEY_B | KEY_START | KEY_SELECT | KEY_L | KEY_R);
    if (!mv && !hit) continue;
    dirty = true;

    if (hit & KEY_B) return true;
    else if (mv & KEY_DOWN) { if (vis) sel = (sel + 1) % vis; }
    else if (mv & KEY_UP)   { if (vis) sel = (sel == 0) ? vis - 1 : sel - 1; }
    else if (hit & KEY_L)   { sel -= ROWS; if (sel < 0) sel = 0; }
    else if (hit & KEY_R)   { sel += ROWS; if (sel >= vis) sel = vis - 1; }
    else if (hit & KEY_SELECT) {
      if (confirm("Empty Trash?", "Permanently erases ALL items")) {
        if (trash_empty()) return true;                /* gone -> back to browser */
        vis = trash_visible();                         /* failed: refresh count */
      }
    }
    else if (hit & (KEY_A | KEY_START)) {
      FsEntry* se = &g_entries[g_trash_map[sel]];
      char stored[FS_NAME_CAP];
      strcpy(stored, se->name);
      char origin[PATH_MAX], nb[128], ob[128];
      ui_clear();
      ui_text(6, 36, UI_TITLE, "TRASHED ITEM");
      ui_truncate(nb, stored, 28);
      ui_text(6, 56, UI_TEXT, nb);
      if (read_origin(stored, origin)) { ui_truncate(ob, origin, 29); ui_text(6, 72, UI_DIM, ob); }
      ui_text(6, 98,  UI_OK,   "A = Restore");
      ui_text(6, 112, UI_WARN, "START = Delete forever");
      ui_text(6, 130, UI_DIM,  "B = cancel");
      u16 k = wait_keys(KEY_A | KEY_START | KEY_B);
      bool acted = false;
      if (k & KEY_A) acted = do_restore(stored);
      else if (k & KEY_START) { if (confirm("Delete forever?", "This cannot be undone")) acted = do_purge(stored); }
      if (acted) {
        vis = trash_scan();
        if (vis == 0) { msg_screen("Trash is empty", UI_DIM, "All items handled"); return true; }
        if (sel >= vis) sel = vis - 1;
      }
    }
  }
}

/* Recursively total a folder's bytes + file/subfolder counts and show them.
 * Read-only — works on both carts. Returns false (no listing change). */
static bool do_foldersize(const FsEntry* e) {
  char np[PATH_MAX];
  if (!path_join(g_cwd, e->name, np)) { msg_screen("Path too long", UI_WARN, NULL); return false; }
  show_msg("Computing size...", e->name);
  uint64_t bytes = 0; uint32_t files = 0, dirs = 0;
  FRESULT fr = fsop_dirsize(np, &bytes, &files, &dirs);
  if (fr != FR_OK) { msg_screen("Size failed", UI_WARN, fr_str(fr)); return false; }

  char szb[24], l1[64], l2[64], nb[128];
  human_size(bytes, szb);
  if (bytes >> 32) siprintf(l1, "%s", szb);
  else             siprintf(l1, "%s  (%lu bytes)", szb, (unsigned long)bytes);
  siprintf(l2, "%lu files, %lu folders", (unsigned long)files, (unsigned long)dirs);
  ui_clear();
  ui_text(6, 40, UI_TITLE, "FOLDER SIZE");
  ui_truncate(nb, e->name, 28);
  ui_text(6, 58, UI_DIM, nb);
  ui_text(6, 78, UI_OK, l1);
  ui_text(6, 92, UI_TEXT, l2);
  ui_text(6, 120, UI_DIM, "B = back");
  wait_keys(KEY_B);
  return false;
}

/* ---- settings menu + reboot (both carts) ------------------------------- */

/* Modal settings editor: UP/DOWN pick a row, LEFT/RIGHT change its value (the
 * theme previews live). A (or START) saves + closes; B cancels and reverts
 * every change (including the live theme preview) — matching the app's B=back
 * idiom. On save, changes are pushed to the live globals and persisted to
 * CFG_PATH (Omega-only). Returns true if the listing must be re-read (sort or
 * show-hidden changed); false means a plain repaint suffices. */
static bool settings_menu(void) {
  enum { S_THEME, S_SORT, S_HIDDEN, S_CONFDEL, S_TRASH, S_VIEWER,
         S_JUMP, S_KDELAY, S_KSPEED, S_FREE, S_RESET, S_COUNT };
  Settings snap = g_set;             /* snapshot for B = cancel/revert */
  u16 saved_mask = ui_get_repeat_mask();
  ui_set_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
  int sel = 0;
  bool dirty = true, save = false;
  while (1) {
    if (dirty) {
      ui_clear();
      ui_text(2, HDR_Y, UI_TITLE, "SETTINGS");
      ui_panel(0, 12, 240, 132, UI_PANEL, UI_BORDER);
      char row[64];
      for (int i = 0; i < S_COUNT; i++) {
        switch (i) {
          case S_THEME:   siprintf(row, "Theme:        %s", theme_name(g_set.theme)); break;
          case S_SORT:    siprintf(row, "Sort:         %s", sort_label_of(g_set.sort_key, g_set.sort_rev)); break;
          case S_HIDDEN:  siprintf(row, "Show hidden:  %s", g_set.show_hidden ? "ON" : "off"); break;
          case S_CONFDEL: siprintf(row, "Confirm del:  %s", g_set.confirm_delete ? "ON" : "off"); break;
          case S_TRASH:   siprintf(row, "Delete to:    %s", g_set.delete_to_trash ? "Trash" : "Permanent"); break;
          case S_VIEWER:  siprintf(row, "Open files:   %s", g_set.viewer_hex ? "Hex" : "Text"); break;
          case S_JUMP:    siprintf(row, "L/R jump:     %d rows", g_set.jump); break;
          case S_KDELAY:  siprintf(row, "Key delay:    %d frames", g_set.key_delay); break;
          case S_KSPEED:  siprintf(row, "Key repeat:   %d frames", g_set.key_speed); break;
          case S_FREE:    siprintf(row, "Free space:   %s", free_unit_name(g_set.free_unit)); break;
          case S_RESET:   siprintf(row, "Reset to defaults   (L/R)"); break;
        }
        ui_text_sel(4, 16 + i * 12, 232, i == sel, UI_TEXT, row);
      }
      ui_text(2, FOOT_Y, UI_DIM, "UD pick LR chg A save B back");
      dirty = false;
    }
    vsync();
    /* key_repeat mutates an internal counter, so call it once/frame and mask.
     * Numeric rows (jump / key timings) auto-repeat on a held L/R; the theme,
     * sort and on/off toggles step once per press so they don't whip past the
     * target value. */
    bool numeric = (sel == S_JUMP || sel == S_KDELAY || sel == S_KSPEED);
    u16 rep = key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    u16 hit = key_hit(KEY_A | KEY_B | KEY_START | KEY_LEFT | KEY_RIGHT);
    u16 nav = rep & (KEY_UP | KEY_DOWN);
    u16 chg = numeric ? (rep & (KEY_LEFT | KEY_RIGHT)) : (hit & (KEY_LEFT | KEY_RIGHT));
    if (!nav && !hit && !chg) continue;
    dirty = true;
    if (hit & (KEY_A | KEY_START)) { save = true;  break; }   /* save + close   */
    else if (hit & KEY_B)          { save = false; break; }   /* cancel + revert */
    else if (nav & KEY_DOWN) sel = (sel + 1) % S_COUNT;
    else if (nav & KEY_UP)   sel = (sel == 0) ? S_COUNT - 1 : sel - 1;
    else if (chg) {
      int d = (chg & KEY_RIGHT) ? 1 : -1;
      switch (sel) {
        case S_THEME:   g_set.theme = (g_set.theme + THEME_COUNT + d) % THEME_COUNT;
                        theme_apply(g_set.theme); break;          /* live preview */
        case S_SORT: {  int s = (g_set.sort_key * 2 + (g_set.sort_rev ? 1 : 0) + 6 + d) % 6;
                        g_set.sort_key = s / 2; g_set.sort_rev = (s & 1) != 0; break; }
        case S_HIDDEN:  g_set.show_hidden     = !g_set.show_hidden;     break;
        case S_CONFDEL: g_set.confirm_delete  = !g_set.confirm_delete;  break;
        case S_TRASH:   g_set.delete_to_trash = !g_set.delete_to_trash; break;
        case S_VIEWER:  g_set.viewer_hex      = !g_set.viewer_hex;      break;
        case S_JUMP:    g_set.jump      = clampi(g_set.jump + d, 1, 99);    break;
        case S_KDELAY:  g_set.key_delay = clampi(g_set.key_delay + d, 2, 60); break;
        case S_KSPEED:  g_set.key_speed = clampi(g_set.key_speed + d, 1, 30); break;
        case S_FREE:    g_set.free_unit = (g_set.free_unit + FREE_UNIT_COUNT + d) % FREE_UNIT_COUNT; break;
        case S_RESET: { char keep[256]; strcpy(keep, g_set.last_dir);   /* reset prefs, keep last folder */
                        cfg_defaults(); strcpy(g_set.last_dir, keep);
                        theme_apply(g_set.theme); break; }              /* B still cancels/reverts */
      }
    }
  }
  ui_set_repeat_mask(saved_mask);                /* restore the caller's mask */

  if (!save) {                       /* cancel: revert everything incl. the theme */
    g_set = snap;
    theme_apply(g_set.theme);
    return false;
  }
  /* save: push to the live browser globals, persist (write is Omega-only), and
   * report whether a re-list is needed (sort/hidden changed) vs repaint-only. */
  bool need_rescan = (snap.show_hidden != g_set.show_hidden) ||
                     (snap.sort_key   != g_set.sort_key) ||
                     (snap.sort_rev   != g_set.sort_rev);
  g_sortkey = (FsSortKey)g_set.sort_key;
  g_sortrev = g_set.sort_rev;
  key_repeat_limits(g_set.key_delay, g_set.key_speed);
  if (can_write()) cfg_save(CFG_PATH);
  return need_rescan;
}

/* Reboot toward the flashcart loader/menu. Experimental: may land on the menu
 * or just restart the tool (see flashcartio_reboot). No data risk — invoked
 * only while idle and the tool keeps no unsaved SRAM. Persists settings first
 * so a successful reboot keeps the last folder + preferences. Does not return
 * on success. */
static void do_reboot(void) {
  if (!confirm("Reboot to loader?", "Experimental - may just restart")) return;
  strcpy(g_set.last_dir, g_cwd);
  if (can_write()) cfg_save(CFG_PATH);
  show_msg("Rebooting...", NULL);
  VBlankIntrWait();                 /* let the message paint before we tear down */
  flashcartio_reboot();             /* no return on a real flashcart */
}

/* Defined below; the actions menu's "View" item opens it. Returns true if the
 * file was modified (a hex save), so the caller can refresh the listing. */
static bool file_viewer(const char* path, const char* name, uint64_t size);
static bool view_image(const char* path, const char* name, uint64_t size);

/* Open-by-extension dispatch: a registry mapping a file extension to a viewer.
 * The DEFAULT/fallback is always the universal hex/text file_viewer (an entry is
 * listed here only when a real handler exists), so "View (hex/text)" is always
 * available and Open never dispatches to a stub. New type = one view_* function
 * + one row here. Handlers share file_viewer's (path,name,size)->modified shape. */
typedef bool (*OpenHandler)(const char* path, const char* name, uint64_t size);
typedef struct { const char* ext; OpenHandler fn; const char* label; } OpenEntry;
static const OpenEntry g_openers[] = {
  { "bmp", view_image, "View image" },
};
static const OpenEntry* opener_for(const char* name) {
  for (unsigned i = 0; i < sizeof(g_openers) / sizeof(g_openers[0]); i++)
    if (fsop_ext_is(name, g_openers[i].ext)) return &g_openers[i];
  return 0;
}

/* Recursive keyword search from the current directory. Prompts for a keyword,
 * walks the subtree (read-only, both carts), then shows a scrollable list of
 * matches. Picking one navigates the browser to the match's folder and selects
 * it (via g_find_sel). Returns true if it navigated (caller rescans), false on
 * cancel / no input / no matches. */
static bool find_modal(void) {
  char kw[64];
  if (!osk_input("Find (name has):", NULL, kw, sizeof(kw))) return false;
  if (!kw[0]) return false;

  show_msg("Searching...", g_cwd);
  int n = fsop_find(g_cwd, kw, g_find_paths, g_find_isdir, FIND_MAX, &g_find_trunc);
  if (n < 0) {                                  /* f_opendir(g_cwd) failed: I/O error, not empty */
    log_line("find '%s' in %s: open failed", kw, g_cwd);
    log_flush_to_sd(LOG_PATH);
    msg_screen("Search failed", UI_WARN, g_cwd);
    return false;
  }
  g_find_count = n;
  log_line("find '%s' in %s: %d%s", kw, g_cwd, g_find_count, g_find_trunc ? " (capped)" : "");
  log_flush_to_sd(LOG_PATH);
  if (g_find_count == 0) { msg_screen("No matches", UI_DIM, kw); return false; }

  int sel = 0, top = 0;
  bool dirty = true;
  while (1) {
    if (sel >= g_find_count) sel = g_find_count - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
    if (top > g_find_count - LIST_ROWS) top = g_find_count - LIST_ROWS;
    if (top < 0) top = 0;

    if (dirty) {
      ui_clear();
      char hdr[128], kn[128];
      ui_truncate(kn, kw, 14);
      siprintf(hdr, "Find '%s'  %d%s", kn, g_find_count, g_find_trunc ? "+" : "");
      ui_truncate(hdr, hdr, 29);
      ui_text(2, HDR_Y, UI_TITLE, hdr);
      ui_panel(0, BOX_Y, 240, BOX_H, UI_PANEL, UI_BORDER);
      for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_find_count) break;
        char row[140], nm[128];
        ui_truncate(nm, base_name(g_find_paths[idx]), g_find_isdir[idx] ? 27 : 28);
        if (g_find_isdir[idx]) siprintf(row, "%s/", nm); else siprintf(row, "%s", nm);
        ui_text_sel(3, ROW0_Y + r * ROW_H, 234, idx == sel,
                    g_find_isdir[idx] ? UI_DIRCLR : UI_SAVECLR, row);
      }
      if (top > 0)                        ui_text(232, ROW0_Y, UI_DIM, "^");
      if (top + LIST_ROWS < g_find_count) ui_text(232, ROW0_Y + (LIST_ROWS - 1) * ROW_H, UI_DIM, "v");
      char fp[128];
      ui_truncate(fp, g_find_paths[sel], 29);
      ui_text(2, DETAIL_NAME_Y, UI_SELTEXT, fp);          /* full path of the selection */
      ui_text(2, FOOT_Y, UI_DIM, "A go to  B back");
      dirty = false;
    }
    vsync();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_A | KEY_B | KEY_START);
    if (!mv && !hit) continue;
    dirty = true;
    if (hit & KEY_B) return false;
    else if (mv & KEY_DOWN) sel = (sel + 1) % g_find_count;
    else if (mv & KEY_UP)   sel = (sel == 0) ? g_find_count - 1 : sel - 1;
    else if (hit & (KEY_A | KEY_START)) {
      char parent[PATH_MAX];
      parent_of(g_find_paths[sel], parent);
      strcpy(g_cwd, parent);
      strcpy(g_find_sel, base_name(g_find_paths[sel]));
      return true;                                        /* navigated */
    }
  }
}

/* Per-entry action menu. Returns true if the listing must be rescanned (a
 * mutation succeeded, or a find navigated). Write actions appear only on
 * EZ-Flash Omega; on EverDrive the tool stays read-only. `e` is NULL on the
 * [..] row. Find / Settings / Reboot are always present (both carts), so they
 * are reachable even on an empty directory / the [..] row. */
static bool actions_menu(const FsEntry* e) {
  enum { A_OPEN, A_VIEW, A_INFO, A_FOLDERSIZE, A_FIND, A_RENAME, A_COPY, A_CUT, A_DUPLICATE,
         A_PASTE, A_RDO, A_HID, A_DELETE, A_NEWFILE, A_MKDIR, A_SELECT, A_TRASH,
         A_SETTINGS, A_REBOOT };
  int  ids[24];
  char labels[24][32];
  int  ni = 0;

  if (e) {
    if (!e->is_dir) {
      const OpenEntry* op = opener_for(e->name);          /* type-specific viewer, if any */
      if (op) { ids[ni] = A_OPEN; strcpy(labels[ni++], op->label); }
      ids[ni] = A_VIEW; strcpy(labels[ni++], "View (hex/text)");
    }
    ids[ni] = A_INFO; strcpy(labels[ni++], "Info / properties");
    if (e->is_dir) { ids[ni] = A_FOLDERSIZE; strcpy(labels[ni++], "Folder size"); }
  }
  ids[ni] = A_FIND; strcpy(labels[ni++], "Find...");   /* recursive search, both carts */
  if (can_write()) {
    if (e) {
      ids[ni] = A_RENAME;    strcpy(labels[ni++], "Rename");
      ids[ni] = A_COPY;      strcpy(labels[ni++], "Copy");
      ids[ni] = A_CUT;       strcpy(labels[ni++], "Cut");
      ids[ni] = A_DUPLICATE; strcpy(labels[ni++], "Duplicate");
    }
    if (g_clip_op != CLIP_NONE) {
      ids[ni] = A_PASTE; siprintf(labels[ni++], "Paste %s here", g_clip_op == CLIP_CUT ? "(move)" : "(copy)");
    }
    if (e) {
      ids[ni] = A_RDO; siprintf(labels[ni++], "Read-only: %s", (e->attrib & AM_RDO) ? "ON" : "off");
      ids[ni] = A_HID; siprintf(labels[ni++], "Hidden: %s",    (e->attrib & AM_HID) ? "ON" : "off");
      ids[ni] = A_DELETE;
      strcpy(labels[ni++], g_set.delete_to_trash
               ? (e->is_dir ? "Move folder to Trash" : "Move file to Trash")
               : (e->is_dir ? "Delete folder (perm)" : "Delete file (perm)"));
    }
    ids[ni] = A_NEWFILE; strcpy(labels[ni++], "New file here");
    ids[ni] = A_MKDIR;   strcpy(labels[ni++], "New folder here");
    ids[ni] = A_SELECT;  strcpy(labels[ni++], "Select multiple");
    ids[ni] = A_TRASH;   strcpy(labels[ni++], "Trash (recycle bin)...");
  }
  /* always available, both carts (settings persist Omega-only) */
  ids[ni] = A_SETTINGS; strcpy(labels[ni++], "Settings...");
  ids[ni] = A_REBOOT;   strcpy(labels[ni++], "Reboot to loader...");

  /* The list can hold up to ~19 items (ids[24] cap) but the panel fits 9; scroll
   * a sel/top window like the browser list so rows never spill past the box. */
  const int VIS = 9, ROW0 = 18, PITCH = 12;
  int sel = 0, top = 0;
  bool dirty = true;
  while (1) {
    if (ni == 0) sel = 0; else if (sel >= ni) sel = ni - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + VIS) top = sel - VIS + 1;
    if (top > ni - VIS) top = ni - VIS;
    if (top < 0) top = 0;

    if (dirty) {
      ui_clear();
      char hb[128], nb[128], hh[128];   /* UTF-8 name -> ~4 bytes/col; size for worst case */
      ui_truncate(nb, e ? e->name : "(parent)", 18);
      siprintf(hb, "Actions: %s", nb);
      ui_truncate(hh, hb, 29);
      ui_text(2, HDR_Y, UI_TITLE, hh);
      ui_panel(0, 12, 240, 118, UI_PANEL, UI_BORDER);
      for (int r = 0; r < VIS; r++) {
        int idx = top + r;
        if (idx >= ni) break;
        ui_text_sel(4, ROW0 + r * PITCH, 232, idx == sel, UI_TEXT, labels[idx]);
      }
      if (top > 0)         ui_text(230, ROW0, UI_DIM, "^");                       /* more above */
      if (top + VIS < ni)  ui_text(230, ROW0 + (VIS - 1) * PITCH, UI_DIM, "v");   /* more below */
      if (!can_write() && ni <= VIS)
        ui_text(4, ROW0 + ni * PITCH + 6, UI_DIM, "Writes need EZ-Flash Omega");
      ui_text(2, FOOT_Y, UI_DIM, "A do   B back");
      dirty = false;
    }
    vsync();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_A | KEY_B);
    if (!mv && !hit) continue;
    dirty = true;
    if (hit & KEY_B) return false;
    else if (mv & KEY_DOWN) { if (ni) sel = (sel + 1) % ni; }
    else if (mv & KEY_UP)   { if (ni) sel = (sel == 0) ? ni - 1 : sel - 1; }
    else if ((hit & KEY_A) && ni) {
      switch (ids[sel]) {
        case A_OPEN: {     /* type-specific viewer (e.g. image) via the registry */
          const OpenEntry* op = opener_for(e->name);
          char np[PATH_MAX];
          if (op && path_join(g_cwd, e->name, np)) return op->fn(np, e->name, e->size);
          msg_screen("Path too long", UI_WARN, NULL);
          break;
        }
        case A_VIEW: {     /* open the hex/text viewer (read-only unless hex-edited) */
          char np[PATH_MAX];
          if (path_join(g_cwd, e->name, np)) return file_viewer(np, e->name, e->size);
          msg_screen("Path too long", UI_WARN, NULL);
          break;
        }
        case A_INFO:       properties_screen(e);              break;
        case A_FOLDERSIZE: do_foldersize(e);                  break;
        case A_FIND:       return find_modal();  /* navigated -> true; cancel -> false */
        case A_RENAME:     if (do_rename(e))            return true; break;
        case A_COPY:       do_copy(e);                  return false;  /* set clipboard, show indicator */
        case A_CUT:        do_cut(e);                   return false;
        case A_DUPLICATE:  if (do_duplicate(e))         return true; break;
        case A_PASTE:      if (do_paste())              return true; break;
        case A_RDO:        if (do_chmod_toggle(e, AM_RDO)) return true; break;
        case A_HID:        if (do_chmod_toggle(e, AM_HID)) return true; break;
        case A_DELETE:     if (do_delete(e))            return true; break;
        case A_NEWFILE:    if (do_newfile())            return true; break;
        case A_MKDIR:      if (do_mkdir())              return true; break;
        case A_SELECT:     g_selmode = true; memset(g_marked, 0, sizeof(g_marked)); return false;
        case A_TRASH:      return trash_modal();    /* restore/purge/empty the recycle bin */
        case A_SETTINGS:   return settings_menu();  /* true only if sort/hidden changed */
        case A_REBOOT:     do_reboot();     break;  /* returns only if cancelled */
      }
      dirty = true;
    }
  }
}

/* ---- read-only file viewer (hex / text) -------------------------------- */

#define VIEW_ROWS 15
#define HEX_BPR   8
#define TXT_BPR   28
static u8 EWRAM_BSS g_viewbuf[VIEW_ROWS * TXT_BPR];   /* 420 B page window */

/* Hex-editor state: pending edits are kept as a sparse overlay (offset -> new
 * byte) on top of the on-disk window, and written out together via the verified
 * fsop_apply_edits. The file itself is untouched until the user saves. */
#define HEX_EDIT_MAX 512
static HexEdit  EWRAM_BSS g_edits[HEX_EDIT_MAX];
static int                g_nedits = 0;
static bool               g_editing = false;   /* hex EDIT mode active   */
static uint32_t           g_ecur = 0;          /* edit cursor: abs offset */

static int edit_find(uint32_t off) {
  for (int i = 0; i < g_nedits; i++) if (g_edits[i].off == off) return i;
  return -1;
}
static uint8_t edit_get(uint32_t off, uint8_t base) {
  int i = edit_find(off);
  return (i >= 0) ? g_edits[i].val : base;
}
/* Returns false only when a NEW edit can't be stored (buffer full) so the
 * caller can warn — a silently dropped edit must never reach a save. */
static bool edit_set(uint32_t off, uint8_t val, uint8_t base) {
  int i = edit_find(off);
  if (val == base) {                           /* reverted to original -> drop edit */
    if (i >= 0) g_edits[i] = g_edits[--g_nedits];
    return true;
  }
  if (i >= 0) { g_edits[i].val = val; return true; }
  if (g_nedits < HEX_EDIT_MAX) { g_edits[g_nedits].off = off; g_edits[g_nedits].val = val; g_nedits++; return true; }
  return false;                                /* HEX_EDIT_MAX reached */
}

/* Word-wrapped text layout for the viewer's TEXT mode: flows `got` bytes into up
 * to VIEW_ROWS lines of <= TXT_COLS columns, breaking at LF (CR skipped), tabs
 * expanded to 4-col stops, non-printable -> '.', wrapping long lines at the last
 * space (else hard char-wrap). The font is ASCII-only (sys8), so high bytes show
 * as '.'. Navigation stays byte-offset based, so a page boundary may split a
 * line — acceptable for a reader. */
#define TXT_COLS 29
static void render_text_wrapped(const uint8_t* buf, uint32_t got) {
  char line[TXT_COLS + 1];
  int lc = 0, row = 0;
  for (uint32_t i = 0; i < got && row < VIEW_ROWS; ) {
    uint8_t ch = buf[i++];
    if (ch == '\r') continue;
    if (ch == '\n') { line[lc] = 0; ui_text(2, 10 + row * 8, UI_TEXT, line); row++; lc = 0; continue; }
    if (ch == '\t') { int adv = 4 - (lc % 4); while (adv-- > 0 && lc < TXT_COLS) line[lc++] = ' '; }
    else            { line[lc++] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '.'; }
    if (lc >= TXT_COLS) {                          /* wrap: prefer the last space */
      int sp = -1;
      for (int k = lc - 1; k > 0; k--) if (line[k] == ' ') { sp = k; break; }
      int br = (sp > 0) ? sp : lc;                 /* break column (drop the space) */
      char carry[TXT_COLS + 1]; int cc = 0;
      for (int k = (sp > 0 ? sp + 1 : br); k < lc; k++) carry[cc++] = line[k];
      line[br] = 0; ui_text(2, 10 + row * 8, UI_TEXT, line); row++;
      lc = 0;
      for (int k = 0; k < cc && lc < TXT_COLS; k++) line[lc++] = carry[k];
    }
  }
  if (row < VIEW_ROWS && lc > 0) { line[lc] = 0; ui_text(2, 10 + row * 8, UI_TEXT, line); }
}

static void render_view(const char* name, uint64_t size, uint64_t off,
                        const uint8_t* buf, uint32_t got, bool hex) {
  ui_clear();
  char line[128], nb[128], hbuf[96];   /* UTF-8 name -> ~4 bytes/col; size for worst case */
  ui_truncate(nb, name, 20);
  siprintf(hbuf, "%s [%s]", nb, g_editing ? "EDIT" : hex ? "HEX" : "TXT");
  ui_truncate(line, hbuf, 29);
  ui_text(2, HDR_Y, g_editing ? UI_WARN : UI_TITLE, line);

  if (got == 0) {
    ui_text(8, 12, UI_DIM, "(empty file)");
  } else if (!hex) {
    render_text_wrapped(buf, got);                        /* word-wrapped text mode */
  } else {
    for (int r = 0; r < VIEW_ROWS; r++) {
      uint32_t ro = (uint32_t)(r * HEX_BPR);
      if (ro >= got) break;
      int y = 10 + r * 8;
      char ofs[12];
      siprintf(ofs, "%05lX:", (unsigned long)(off + ro));
      ui_text(2, y, UI_DIM, ofs);                         /* offset label */
      for (int i = 0; i < HEX_BPR && ro + (uint32_t)i < got; i++) {
        uint32_t a = (uint32_t)(off + ro + i);
        uint8_t  b = edit_get(a, buf[ro + i]);
        char hh[3]; siprintf(hh, "%02X", b);
        int bx = 50 + i * 23;
        if (g_editing && a == g_ecur) {                   /* editable byte: inverse video (theme-aware) */
          m3_rect(bx, y, bx + 16, y + UI_ROW_H, UI_TEXT);
          ui_text(bx, y, UI_BG, hh);
        } else {
          ui_text(bx, y, edit_find(a) >= 0 ? UI_WARN : UI_SAVECLR, hh);
        }
      }
    }
  }

  char st[48], stt[48];
  if (g_editing) {
    uint8_t cur = edit_get(g_ecur, (g_ecur >= off && g_ecur < off + got) ? buf[g_ecur - off] : 0);
    siprintf(st, "EDIT @%lX = %02X  %d edit%s", (unsigned long)g_ecur, cur,
             g_nedits, g_nedits == 1 ? "" : "s");
    ui_truncate(stt, st, 29);
    ui_text(2, STATUS_Y, UI_WARN, stt);
    ui_text(2, FOOT_Y, UI_DIM, "L/R val  ST save  SE undo  B exit");
  } else {
    unsigned long pct = size ? (unsigned long)((off * 100) / size) : 100;
    siprintf(st, "off %lu/%lu  %lu%%", (unsigned long)off, (unsigned long)size, pct);
    ui_truncate(stt, st, 29);
    ui_text(2, STATUS_Y, UI_OK, stt);
    ui_text(2, FOOT_Y, UI_DIM, "A hex/txt  ST edit  SE goto");
  }
}

/* Parse a hex string (optional 0x prefix) into a 64-bit value, stopping at the
 * first non-hex char. Used by the viewer's "go to offset". */
static uint64_t parse_hex64(const char* s) {
  while (*s == ' ' || *s == '\t') s++;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  uint64_t v = 0;
  for (; *s; s++) {
    char c = *s; int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else break;
    v = (v << 4) | (uint64_t)d;
  }
  return v;
}

/* Page-windowed viewer: keeps a byte offset and re-reads one screen worth of
 * bytes on each move (f_lseek + f_read into an EWRAM buffer), so it handles
 * files far larger than RAM. Reads happen BEFORE render() — never mid-transfer
 * with the ROM unmapped. */
static bool file_viewer(const char* path, const char* name, uint64_t size) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    show_msg("Open failed", base_name(path));
    ui_text(6, 100, UI_DIM, "B = back");
    wait_keys(KEY_B);
    return false;
  }

  /* Let the d-pad + shoulders auto-repeat while in the viewer (paging and, in
   * edit mode, cursor/value changes); restored to the caller's mask on exit. */
  u16 saved_mask = ui_get_repeat_mask();
  ui_set_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R);

  bool hex = g_set.viewer_hex, quit = false, fopen = true;   /* default view per settings */
  bool modified = false;                                     /* a hex save succeeded */
  uint64_t off = 0;
  uint32_t got = 0;
  bool dirty = true;
  g_editing = false; g_nedits = 0; g_ecur = 0;

  while (!quit) {
    int bpr = hex ? HEX_BPR : TXT_BPR;
    uint32_t page = (uint32_t)(VIEW_ROWS * bpr);

    if (g_editing) {                         /* scroll window to keep the cursor visible */
      uint32_t crow = (g_ecur / HEX_BPR) * HEX_BPR;
      if ((uint64_t)g_ecur < off) off = crow;
      else if ((uint64_t)g_ecur >= off + page) off = crow - (uint32_t)((VIEW_ROWS - 1) * HEX_BPR);
    }
    if (size == 0) off = 0;
    else { uint64_t mo = ((size - 1) / bpr) * bpr; if (off > mo) off = mo; }

    if (dirty) {
      UINT br = 0;
      if (f_lseek(&f, off) == FR_OK) f_read(&f, g_viewbuf, page, &br);
      got = (uint32_t)br;
      render_view(name, size, off, g_viewbuf, got, hex);
      dirty = false;
    }

    vsync();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R);
    u16 hit = key_hit(KEY_A | KEY_B | KEY_START | KEY_SELECT);
    if (!mv && !hit) continue;
    dirty = true;

    if (!g_editing) {
      if (hit & KEY_B)          { quit = true; }
      else if (hit & KEY_A)     { hex = !hex; }                /* toggle hex/text */
      else if (mv & KEY_DOWN)   { off += (uint32_t)bpr; }
      else if (mv & KEY_UP)     { off = (off >= (uint32_t)bpr) ? off - (uint32_t)bpr : 0; }
      else if (mv & KEY_R)      { off += page; }
      else if (mv & KEY_L)      { off = (off >= page) ? off - page : 0; }
      else if (mv & KEY_LEFT)   { off = 0; }
      else if (mv & KEY_RIGHT)  { off = (size > 0) ? (size - 1) : 0; }
      else if (hit & KEY_SELECT) {                             /* jump to a hex offset */
        char in[20];
        if (size > 0 && osk_input("Go to offset (hex):", NULL, in, sizeof(in))) {
          uint64_t t = parse_hex64(in);
          if (t > size - 1) t = size - 1;
          off = (t / (uint32_t)bpr) * (uint32_t)bpr;           /* align to a row */
        }
      }
      else if (hit & KEY_START) {                              /* enter EDIT mode */
        if (!can_write()) msg_screen("Read-only", UI_WARN, "Editing needs EZ-Flash Omega");
        else if (size == 0) msg_screen("Empty file", UI_DIM, "Nothing to edit");
        else {
          hex = true;
          off = (off / HEX_BPR) * HEX_BPR;
          g_editing = true; g_nedits = 0;
          g_ecur = (uint32_t)((off < size) ? off : 0);
        }
      }
    } else {
      /* EDIT mode (hex): d-pad moves the byte cursor, L/R change its value. */
      uint32_t maxb = (size - 1 > 0xFFFFFFFEull) ? 0xFFFFFFFEu : (uint32_t)(size - 1);
      if (g_ecur > maxb) g_ecur = maxb;
      if (mv & KEY_LEFT)        { if (g_ecur > 0) g_ecur--; }
      else if (mv & KEY_RIGHT)  { if (g_ecur < maxb) g_ecur++; }
      else if (mv & KEY_UP)     { if (g_ecur >= HEX_BPR) g_ecur -= HEX_BPR; }
      else if (mv & KEY_DOWN)   { if (g_ecur + HEX_BPR <= maxb) g_ecur += HEX_BPR; }
      else if (mv & (KEY_L | KEY_R)) {                         /* change the cursor byte */
        if (g_ecur >= off && g_ecur < off + got) {
          uint8_t base = g_viewbuf[g_ecur - off];
          uint8_t cur  = edit_get(g_ecur, base);
          if (!edit_set(g_ecur, (mv & KEY_R) ? (uint8_t)(cur + 1) : (uint8_t)(cur - 1), base))
            msg_screen("Edit buffer full", UI_WARN, "Save or undo first");
        }
      }
      else if (hit & KEY_SELECT) { g_nedits = 0; }             /* undo all pending edits */
      else if (hit & KEY_START) {                              /* SAVE (verified, backed up) */
        if (g_nedits == 0) { g_editing = false; }
        else if (confirm("Save edits to file?", "Original kept as .bak~")) {
          show_msg("Saving...", name);
          f_close(&f); fopen = false;
          FRESULT fr = fsop_apply_edits(path, g_edits, g_nedits);
          log_line("hexedit %s (%d edits) -> %d (%s)", path, g_nedits, fr, fr_str(fr));
          log_flush_to_sd(LOG_PATH);
          if (fr == FR_OK) {
            g_nedits = 0; g_editing = false; modified = true;
            msg_screen("Saved", UI_OK, "Original kept as .bak~");
          } else {
            FILINFO chk;                        /* rare: original gone -> point at recovery files */
            if (f_stat(path, &chk) != FR_OK)
              msg_screen("Save failed - RECOVER", UI_WARN, "see name.bak~ and name.hexnew~");
            else
              msg_screen("Save failed", UI_WARN, fr_str(fr));
          }
          if (f_open(&f, path, FA_READ) == FR_OK) fopen = true; else quit = true;  /* reopen saved file */
        }
      }
      else if (hit & KEY_B) {                                  /* leave EDIT mode */
        if (g_nedits == 0 || confirm("Discard edits?", "Unsaved changes lost")) {
          g_editing = false; g_nedits = 0;
        }
      }
    }
  }

  if (fopen) f_close(&f);
  g_editing = false; g_nedits = 0;
  ui_set_repeat_mask(saved_mask);            /* restore the caller's repeat mask */
  return modified;
}

/* ---- BMP image viewer -------------------------------------------------- */

static u8  EWRAM_BSS g_imgrow[8192];   /* one BMP source row (padded)        */
static u16 EWRAM_BSS g_imgpal[256];    /* 8-bit BMP palette -> BGR555        */

static u32 rd_u32le(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd_u16le(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }

static void img_msg(const char* l1, const char* l2) {
  ui_clear();
  ui_text(6, 60, UI_WARN, l1);
  if (l2) ui_text(6, 78, UI_DIM, l2);
  ui_text(6, 110, UI_DIM, "B = back");
  wait_keys(KEY_B);
}

/* View a BMP image: uncompressed 8/16/24/32-bit, blitted to the Mode-3 canvas
 * with integer-decimation downscale to fit 240x160 (centered). Read-only — runs
 * on both carts; returns false (no listing change). Every header field is bounds
 * checked (untrusted SD file). Rows are read into EWRAM BEFORE blitting, so no
 * VRAM write happens during an SD transfer. */
static bool view_image(const char* path, const char* name, uint64_t size) {
  (void)size;
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) { img_msg("Open failed", NULL); return false; }

  u8 hdr[54];
  UINT br = 0;
  if (f_read(&f, hdr, 54, &br) != FR_OK || br < 54 || hdr[0] != 'B' || hdr[1] != 'M') {
    f_close(&f); img_msg("Not a BMP", NULL); return false;
  }
  u32 dataoff = rd_u32le(hdr + 10);
  u32 biSize  = rd_u32le(hdr + 14);
  int wImg    = (int)rd_u32le(hdr + 18);
  int hRaw    = (int)rd_u32le(hdr + 22);     /* signed: negative = top-down */
  u16 bpp     = rd_u16le(hdr + 28);
  u32 comp    = rd_u32le(hdr + 30);
  bool topdown = (hRaw < 0);
  int hImg = topdown ? (int)(0u - (u32)hRaw) : hRaw;   /* negate in u32 (INT_MIN-safe) */

  /* We parse v3 BITMAPINFOHEADER (40-byte) fixed offsets; reject older/core
   * headers (e.g. OS/2 12-byte) rather than misread them as garbage dims. */
  if (biSize < 40)                                 { f_close(&f); img_msg("Unsupported BMP", "old/core header"); return false; }
  if (wImg <= 0 || hImg <= 0 || wImg > 16384 || hImg > 16384) { f_close(&f); img_msg("Bad BMP size", NULL); return false; }
  if (dataoff >= f_size(&f))                       { f_close(&f); img_msg("Bad BMP", "bad data offset"); return false; }
  if (comp != 0)                                   { f_close(&f); img_msg("Compressed BMP", "RLE not supported"); return false; }
  if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) { f_close(&f); img_msg("Unsupported BMP", "need 8/16/24/32-bit"); return false; }

  u32 stride = (((u32)wImg * bpp + 31) / 32) * 4;
  if (stride > sizeof(g_imgrow)) { f_close(&f); img_msg("Image too wide", NULL); return false; }

  if (bpp == 8) {                                  /* load the palette -> BGR555 */
    static u8 EWRAM_BSS palraw[1024];
    u32 paloff = 14 + biSize;
    u32 ncol = rd_u32le(hdr + 46);                 /* biClrUsed (0 => 256) */
    if (ncol == 0 || ncol > 256) ncol = 256;
    if (f_lseek(&f, paloff) != FR_OK || f_read(&f, palraw, ncol * 4, &br) != FR_OK || br < ncol * 4) {
      f_close(&f); img_msg("Palette read failed", NULL); return false;
    }
    for (u32 i = 0; i < 256; i++) {
      if (i < ncol) {
        u8 B = palraw[i*4], G = palraw[i*4+1], R = palraw[i*4+2];
        g_imgpal[i] = (u16)((R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10));
      } else g_imgpal[i] = 0;
    }
  }

  int scale = 1;
  while (wImg / scale > 240 || hImg / scale > 160) scale++;   /* integer decimation */
  int ow = wImg / scale, oh = hImg / scale;
  if (ow < 1) ow = 1;
  if (oh < 1) oh = 1;
  int ox0 = (240 - ow) / 2, oy0 = (160 - oh) / 2;

  ui_clear();
  for (int oy = 0; oy < oh; oy++) {
    int imgRow  = oy * scale;                       /* row from the top of the image */
    int fileRow = topdown ? imgRow : (hImg - 1 - imgRow);
    if (f_lseek(&f, dataoff + (u32)fileRow * stride) != FR_OK) break;
    if (f_read(&f, g_imgrow, stride, &br) != FR_OK || br < stride) break;
    u16* dst = &vid_mem[(oy0 + oy) * 240 + ox0];
    for (int ox = 0; ox < ow; ox++) {
      int sx = ox * scale;
      u16 c;
      if (bpp == 8) {
        c = g_imgpal[g_imgrow[sx]];
      } else if (bpp == 16) {                        /* X1R5G5B5 -> GBA BGR555 */
        u16 px = rd_u16le(g_imgrow + sx * 2);
        c = (u16)(((px >> 10) & 31) | (((px >> 5) & 31) << 5) | ((px & 31) << 10));
      } else {                                       /* 24 or 32-bit: BGR(A) */
        int bypp = (bpp == 24) ? 3 : 4;
        const u8* p = g_imgrow + sx * bypp;
        c = (u16)((p[2] >> 3) | ((p[1] >> 3) << 5) | ((p[0] >> 3) << 10));
      }
      dst[ox] = c;
    }
  }
  f_close(&f);

  char nb[128], ft[160], ftt[160];
  ui_truncate(nb, name, 14);
  siprintf(ft, "%s  %dx%d  B=back", nb, wImg, hImg);
  ui_truncate(ftt, ft, 29);
  m3_rect(0, 151, 240, 160, UI_BG);                  /* clear a strip so the footer reads */
  ui_text(2, 152, UI_DIM, ftt);
  wait_keys(KEY_B);
  return false;
}

/* ---- main loop ---------------------------------------------------------- */

/* ---- multi-select batch operations ------------------------------------- */

static int marked_count(void) {
  int c = 0;
  for (int i = 0; i < g_n; i++) if (g_marked[i]) c++;
  return c;
}

static int capture_marked(ClipOp op) {
  clip_reset(op);
  for (int i = 0; i < g_n; i++)
    if (g_marked[i] && !clip_add(g_entries[i].name)) break;   /* stop if clipboard fills */
  return g_clip_count;
}

static bool do_delete_marked(int count) {
  bool trash = g_set.delete_to_trash;
  if (g_set.confirm_delete) {
    char l1[40];
    siprintf(l1, trash ? "Trash %d items?" : "Delete %d items?", count);
    if (!confirm(l1, trash ? "Restore later from Trash" : "Folders include all contents")) return false;
  }
  show_msg(trash ? "Moving to Trash..." : "Deleting...", "marked items");
  int ok = 0, fail = 0;
  for (int i = 0; i < g_n; i++) {
    if (!g_marked[i]) continue;
    char p[PATH_MAX];
    if (!path_join(g_cwd, g_entries[i].name, p)) { fail++; continue; }
    FRESULT fr = trash ? do_trash_path(p) : fsop_delete(p);
    log_line("batch %s %s : %d", trash ? "trash" : "delete", p, fr);
    if (fr == FR_OK) ok++; else fail++;
  }
  log_flush_to_sd(LOG_PATH);
  if (fail) { char m[48]; siprintf(m, "%d ok  %d failed", ok, fail); msg_screen(trash ? "Trash finished" : "Delete finished", UI_WARN, m); }
  return ok > 0;
}

/* Swap the names of exactly two marked siblings via a 3-way rename through a
 * reserved temp: A -> tmp, B -> A, tmp -> B (rolls back on any failure so the
 * pair is never left half-renamed). Selection mode is Omega-only, so this is
 * too. Returns true on success. */
static bool do_swap_names(void) {
  const char *na = NULL, *nb = NULL;
  for (int i = 0; i < g_n; i++) {
    if (!g_marked[i]) continue;
    if      (!na) na = g_entries[i].name;
    else if (!nb) nb = g_entries[i].name;
    else return false;                         /* more than two marked */
  }
  if (!na || !nb) return false;

  char pa[PATH_MAX], pb[PATH_MAX], tmp[PATH_MAX];
  if (!path_join(g_cwd, na, pa) || !path_join(g_cwd, nb, pb)) {
    msg_screen("Path too long", UI_WARN, NULL); return false;
  }
  if (!sidecar_name(pa, tmp)) { msg_screen("Swap failed", UI_WARN, "no free temp name"); return false; }

  show_msg("Swapping names...", NULL);
  FRESULT fr = fsop_rename(pa, tmp);             /* A -> tmp */
  if (fr == FR_OK) {
    fr = fsop_rename(pb, pa);                    /* B -> A */
    if (fr == FR_OK) {
      fr = fsop_rename(tmp, pb);                 /* tmp(old A) -> B */
      if (fr != FR_OK) { fsop_rename(pa, pb); fsop_rename(tmp, pa); }  /* undo B->A, tmp->A */
    } else {
      fsop_rename(tmp, pa);                      /* undo A->tmp */
    }
  }
  log_line("swapnames %s <-> %s : %d (%s)", pa, pb, fr, fr_str(fr));
  log_flush_to_sd(LOG_PATH);
  if (fr != FR_OK) { msg_screen("Swap failed", UI_WARN, fr_str(fr)); return false; }
  return true;
}

/* Batch-actions menu for the marked set (selection mode, Omega-only). Returns
 * true if the listing changed. Copy/Cut capture the marked items to the
 * clipboard and leave selection mode so the user can navigate + paste. "Swap
 * names" appears only when exactly two items are marked. */
static bool batch_menu(int count) {
  enum { B_COPY, B_CUT, B_DELETE, B_SWAP, B_CANCEL };
  int  ids[5], ni = 0;
  char labels[5][24];
  siprintf(labels[ni], "Copy %d", count);   ids[ni++] = B_COPY;
  siprintf(labels[ni], "Cut %d", count);    ids[ni++] = B_CUT;
  siprintf(labels[ni], g_set.delete_to_trash ? "Trash %d" : "Delete %d", count); ids[ni++] = B_DELETE;
  if (count == 2) { strcpy(labels[ni], "Swap names"); ids[ni++] = B_SWAP; }
  strcpy(labels[ni], "Cancel");             ids[ni++] = B_CANCEL;

  int sel = 0;
  bool dirty = true;
  while (1) {
    if (dirty) {
      ui_clear();
      ui_text(2, HDR_Y, UI_TITLE, "BATCH ACTIONS");
      ui_panel(0, 12, 240, 78, UI_PANEL, UI_BORDER);
      for (int i = 0; i < ni; i++) ui_text_sel(4, 18 + i * 12, 232, i == sel, UI_TEXT, labels[i]);
      ui_text(2, FOOT_Y, UI_DIM, "A do   B back");
      dirty = false;
    }
    vsync();
    u16 mv = key_repeat(KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_A | KEY_B);
    if (!mv && !hit) continue;
    dirty = true;
    if (hit & KEY_B) return false;
    else if (mv & KEY_DOWN) sel = (sel + 1) % ni;
    else if (mv & KEY_UP)   sel = (sel == 0) ? ni - 1 : sel - 1;
    else if (hit & KEY_A) {
      switch (ids[sel]) {
        case B_COPY:
        case B_CUT: {
          int cap = capture_marked(ids[sel] == B_COPY ? CLIP_COPY : CLIP_CUT);
          if (cap < count) msg_screen("Clipboard full", UI_WARN, "Not all items captured");
          g_selmode = false;
          return false;                       /* navigate, then Paste */
        }
        case B_DELETE: { bool r = do_delete_marked(count); g_selmode = false; return r; }
        case B_SWAP:   { bool r = do_swap_names();         g_selmode = false; return r; }
        case B_CANCEL: return false;
      }
    }
  }
}

/* After a find navigated g_cwd into the match's folder, select the matched
 * entry by name (no-op if g_find_sel is empty). Called right after the rescan
 * that follows the actions menu. */
static void apply_find_sel(int* sel, int* top) {
  if (!g_find_sel[0]) return;             /* not a find-navigate: leave the cursor put */
  int row = 0;                            /* default: top of the freshly-listed folder */
  for (int i = 0; i < g_n; i++) {
    if (!strcmp(g_entries[i].name, g_find_sel)) {
      row = i + (at_root() ? 0 : 1);      /* +1 for the synthetic [..] row */
      break;
    }
  }
  *sel = row; *top = 0;                    /* if the match isn't listed (e.g. hidden), land at top */
  g_find_sel[0] = 0;
}

static void run_browser(void) {
  int sel = 0, top = 0;
  bool dirty = true;
  rescan();

  while (1) {
    int rows = br_rows();
    if (rows == 0) sel = 0; else if (sel >= rows) sel = rows - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
    if (top < 0) top = 0;

    if (dirty) { render_browser(sel, top); dirty = false; }

    vsync();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_A | KEY_B | KEY_L | KEY_R | KEY_LEFT | KEY_RIGHT |
                      KEY_START | KEY_SELECT);
    if (!mv && !hit) continue;
    dirty = true;

    if (mv & KEY_DOWN)        { if (rows) sel = (sel + 1) % rows; }
    else if (mv & KEY_UP)     { if (rows) sel = (sel == 0) ? rows - 1 : sel - 1; }
    else if (hit & KEY_LEFT)  { sel -= g_set.jump; if (sel < 0) sel = 0; }                       /* jump up   */
    else if (hit & KEY_RIGHT) { sel += g_set.jump; if (sel >= rows) sel = rows ? rows - 1 : 0; } /* jump down */
    else if (hit & KEY_L)     { sel -= LIST_ROWS; if (sel < 0) sel = 0; }                /* page up   */
    else if (hit & KEY_R)     { sel += LIST_ROWS; if (sel >= rows) sel = rows ? rows - 1 : 0; } /* page down */
    else if (g_selmode) {
      /* selection mode: A marks the highlighted entry, SELECT marks all/none,
       * START opens batch actions, B leaves selection mode. */
      if (hit & KEY_A) {
        FsEntry* e = br_entry(sel);
        if (e) { int idx = (int)(e - g_entries); g_marked[idx] = !g_marked[idx]; }
      } else if (hit & KEY_SELECT) {
        bool none = (marked_count() == 0);
        for (int i = 0; i < g_n; i++) g_marked[i] = none;   /* none -> all, else clear */
      } else if (hit & KEY_START) {
        int mc = marked_count();
        if (mc == 0) msg_screen("No items marked", UI_DIM, "A marks the highlighted item");
        else if (batch_menu(mc)) { rescan(); sel = 0; top = 0; }
      } else if (hit & KEY_B) {
        g_selmode = false;
        memset(g_marked, 0, sizeof(g_marked));
      }
    }
    else if (hit & KEY_SELECT) {
      /* SELECT: cycle the 6 sort states: Name/Size/Date x ascending/descending */
      int s = ((int)g_sortkey * 2 + (g_sortrev ? 1 : 0) + 1) % 6;
      g_sortkey = (FsSortKey)(s / 2);
      g_sortrev = (s & 1) != 0;
      fsop_sort(g_entries, g_n, g_sortkey, g_sortrev);
      sel = 0; top = 0;
    }
    else if (hit & KEY_A) {
      /* A: enter a folder, go up on [..], or open the actions menu on a file */
      FsEntry* e = br_entry(sel);
      if (!e) {                                   /* [..] up-entry */
        if (!at_root()) { path_up(); rescan(); sel = 0; top = 0; }
      } else if (e->is_dir) {
        char np[PATH_MAX];
        if (path_join(g_cwd, e->name, np)) {
          if (under_trash(np)) msg_screen("Reserved folder", UI_DIM, "Use the Trash action");  /* never browse the bin */
          else { strcpy(g_cwd, np); rescan(); sel = 0; top = 0; }
        }
      } else {
        if (actions_menu(e)) { rescan(); apply_find_sel(&sel, &top); }   /* file -> actions menu */
      }
    }
    else if (hit & KEY_B) {
      if (!at_root()) { path_up(); rescan(); sel = 0; top = 0; }
    }
    else if (hit & KEY_START) {
      /* START: open the actions menu (file, folder, or [..]) — View lives inside it */
      FsEntry* e = br_entry(sel);
      if (actions_menu(e)) { rescan(); apply_find_sel(&sel, &top); }
    }
  }
}

/* ---- bring-up ----------------------------------------------------------- */

static void init_system(void) {
  irq_init(NULL);
  irq_add(II_VBLANK, NULL);
  ui_init();
  ui_set_repeat_mask(KEY_UP | KEY_DOWN);
  key_repeat_limits(16, 4);   /* hold ~0.27s, then repeat ~15/s */
}

static const char* flashcart_name(void) {
  switch (active_flashcart) {
    case EVERDRIVE_GBA_X5: return "EverDrive GBA X5";
    case EZ_FLASH_OMEGA:   return "EZ-Flash Omega/DE";
    default:               return "none";
  }
}

int main(void) {
  init_system();
  g_cwd[0] = '/'; g_cwd[1] = 0;

  log_init();
  log_line("=== SD File Browser (File-Browser-GBA, Phase 0) ===");
  log_line("mGBA debug log: %s", log_under_mgba() ? "active" : "absent");

  show_msg("Detecting flashcart...", NULL);
  if (!flashcartio_activate()) halt_msg("No flashcart detected!");
  log_line("flashcart: %s", flashcart_name());

  static FATFS fs;
  FRESULT fr = f_mount(&fs, "", 1);
  if (fr != FR_OK) { log_line("f_mount failed (fr=%d)", fr); halt_msg("SD mount failed!"); }
  log_line("SD mounted OK");

  int wr = log_flush_to_sd(LOG_PATH);
  log_line("log flush -> %s", wr == 0 ? "OK" : "FAILED");

  /* load persisted settings (reads on both carts; missing file -> defaults),
   * apply the theme + nav tuning, and reopen the last folder if it still
   * exists (else stay at root). */
  cfg_load(CFG_PATH);
  theme_apply(g_set.theme);
  g_sortkey = (FsSortKey)g_set.sort_key;
  g_sortrev = g_set.sort_rev;
  key_repeat_limits(g_set.key_delay, g_set.key_speed);
  log_line("cfg: theme=%d sort=%d/%d hidden=%d last_dir='%s'",
           g_set.theme, g_set.sort_key, g_set.sort_rev, g_set.show_hidden, g_set.last_dir);
  if (g_set.last_dir[0] == '/' && !under_trash(g_set.last_dir)) {   /* never reopen inside the bin */
    DIR d;
    if (f_opendir(&d, g_set.last_dir) == FR_OK) { f_closedir(&d); strcpy(g_cwd, g_set.last_dir); }
  }

  run_browser();
  return 0;
}
