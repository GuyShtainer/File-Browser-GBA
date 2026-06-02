/*
 * SD File Browser (sd-browser) — a GBA-native microSD file manager.
 *
 * Phase 0 (this build): read-only browser. Runs on BOTH EZ-Flash Omega DE and
 * EverDrive GBA X5 (read works on both). Navigate folders, sort the listing,
 * see free space, and inspect file properties. No writes, no text entry yet —
 * those arrive in Phase 1 (on-screen keyboard + mkdir/delete/attrs, Omega-only).
 *
 * Keys:
 *   UP/DOWN  move (auto-repeat on hold)
 *   LEFT     jump to top of listing      RIGHT  jump to bottom
 *   L        page up                     R      page down
 *   A        open folder / file properties
 *   B        up one folder
 *   START    cycle sort key (Name -> Size -> Date)
 *   SELECT   toggle sort direction (^ ascending / v descending)
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

#define LOG_PATH  "/sdbrowse_log.txt"
#define FS_MAX    256
#define PATH_MAX  256

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
  int r = fsop_list(g_cwd, g_entries, FS_MAX, &g_trunc);
  g_n = (r < 0) ? 0 : r;
  if (r < 0) log_line("opendir %s failed", g_cwd);
  fsop_sort(g_entries, g_n, g_sortkey, g_sortrev);
  uint64_t f = 0, t = 0;
  g_free_ok = (fsop_freespace(g_cwd, &f, &t) == FR_OK);
  g_free = f; g_total = t;
  log_line("scan %s: %d entries%s", g_cwd, g_n, g_trunc ? " (capped)" : "");
}

/* ---- rendering ---------------------------------------------------------- */

/* Spelled-out description of the active sort key + direction, shown on the
 * status bar. Folders always sort before files regardless of this. */
static const char* sort_label(void) {
  switch (g_sortkey) {
    case FS_SORT_SIZE: return g_sortrev ? "Size big-small" : "Size small-big";
    case FS_SORT_DATE: return g_sortrev ? "Date new-old"   : "Date old-new";
    case FS_SORT_NAME:
    default:           return g_sortrev ? "Name Z-A"       : "Name A-Z";
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
    /* elaborate sort label + scroll position + free space */
    if (g_free_ok)
      siprintf(st, "%s %d/%d %luMB%s", sort_label(), rows ? sel + 1 : 0, rows,
               (unsigned long)(g_free >> 20), g_trunc ? " +" : "");
    else
      siprintf(st, "%s %d/%d%s", sort_label(), rows ? sel + 1 : 0, rows,
               g_trunc ? " +" : "");
  }
  ui_truncate(stt, st, 29);
  ui_text(2, STATUS_Y, UI_OK, stt);

  if (g_selmode) {
    ui_text(2, FOOT_Y, UI_OK, "A mark ST all SE batch B exit");
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
  } else {
    ui_text(2, FOOT_Y, UI_DIM, "A open B up SE view ST:sort");
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

static bool do_delete(const FsEntry* e) {
  /* nb/l1 hold a UTF-8 name truncated to N cols -> up to ~4N bytes; size for the worst case. */
  char np[PATH_MAX], l1[128], nb[128];
  if (!path_join(g_cwd, e->name, np)) { msg_screen("Path too long", UI_WARN, NULL); return false; }
  ui_truncate(nb, e->name, 22);
  siprintf(l1, "Delete %s ?", nb);
  if (!confirm(l1, e->is_dir ? "FOLDER + ALL its contents!" : NULL)) return false;
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
static bool is_reserved(const char* name) { return strstr(name, SIDECAR_MARK) != NULL; }

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

/* Per-entry action menu (SELECT). Returns true if the listing must be
 * rescanned (a mutation succeeded). Write actions appear only on EZ-Flash
 * Omega; on EverDrive the tool stays read-only. `e` is NULL on the [..] row. */
static bool actions_menu(const FsEntry* e) {
  enum { A_INFO, A_RENAME, A_COPY, A_CUT, A_PASTE, A_RDO, A_HID, A_DELETE, A_MKDIR, A_SELECT };
  int  ids[11];
  char labels[11][32];
  int  ni = 0;

  if (e) { ids[ni] = A_INFO; strcpy(labels[ni++], "Info / properties"); }
  if (can_write()) {
    if (e) {
      ids[ni] = A_RENAME; strcpy(labels[ni++], "Rename");
      ids[ni] = A_COPY;   strcpy(labels[ni++], "Copy");
      ids[ni] = A_CUT;    strcpy(labels[ni++], "Cut");
    }
    if (g_clip_op != CLIP_NONE) {
      ids[ni] = A_PASTE; siprintf(labels[ni++], "Paste %s here", g_clip_op == CLIP_CUT ? "(move)" : "(copy)");
    }
    if (e) {
      ids[ni] = A_RDO; siprintf(labels[ni++], "Read-only: %s", (e->attrib & AM_RDO) ? "ON" : "off");
      ids[ni] = A_HID; siprintf(labels[ni++], "Hidden: %s",    (e->attrib & AM_HID) ? "ON" : "off");
      ids[ni] = A_DELETE; strcpy(labels[ni++], e->is_dir ? "Delete folder" : "Delete file");
    }
    ids[ni] = A_SELECT; strcpy(labels[ni++], "Select multiple");
    ids[ni] = A_MKDIR;  strcpy(labels[ni++], "New folder here");
  }

  int sel = 0;
  bool dirty = true;
  while (1) {
    if (dirty) {
      ui_clear();
      char hb[128], nb[128], hh[128];   /* UTF-8 name -> ~4 bytes/col; size for worst case */
      ui_truncate(nb, e ? e->name : "(parent)", 18);
      siprintf(hb, "Actions: %s", nb);
      ui_truncate(hh, hb, 29);
      ui_text(2, HDR_Y, UI_TITLE, hh);
      ui_panel(0, 12, 240, 118, UI_PANEL, UI_BORDER);
      for (int i = 0; i < ni; i++)
        ui_text_sel(4, 18 + i * 12, 232, i == sel, UI_TEXT, labels[i]);
      if (!can_write())
        ui_text(4, 18 + ni * 12 + 6, UI_DIM, "Writes need EZ-Flash Omega");
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
        case A_INFO:   properties_screen(e);                 break;
        case A_RENAME: if (do_rename(e))               return true; break;
        case A_COPY:   do_copy(e);                     return false;  /* set clipboard, show indicator */
        case A_CUT:    do_cut(e);                      return false;
        case A_PASTE:  if (do_paste())                 return true; break;
        case A_RDO:    if (do_chmod_toggle(e, AM_RDO)) return true; break;
        case A_HID:    if (do_chmod_toggle(e, AM_HID)) return true; break;
        case A_DELETE: if (do_delete(e))               return true; break;
        case A_MKDIR:  if (do_mkdir())                 return true; break;
        case A_SELECT: g_selmode = true; memset(g_marked, 0, sizeof(g_marked)); return false;
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

static void render_view(const char* name, uint64_t size, uint64_t off,
                        const uint8_t* buf, uint32_t got, bool hex) {
  ui_clear();
  char line[128], nb[128], hbuf[96];   /* UTF-8 name -> ~4 bytes/col; size for worst case */
  ui_truncate(nb, name, 20);
  siprintf(hbuf, "%s [%s]", nb, g_editing ? "EDIT" : hex ? "HEX" : "TXT");
  ui_truncate(line, hbuf, 29);
  ui_text(2, HDR_Y, g_editing ? UI_WARN : UI_TITLE, line);

  int bpr = hex ? HEX_BPR : TXT_BPR;
  if (got == 0) {
    ui_text(8, 12, UI_DIM, "(empty file)");
  } else {
    for (int r = 0; r < VIEW_ROWS; r++) {
      uint32_t ro = (uint32_t)(r * bpr);
      if (ro >= got) break;
      int y = 10 + r * 8;
      if (hex) {
        char ofs[12];
        siprintf(ofs, "%05lX:", (unsigned long)(off + ro));
        ui_text(2, y, UI_DIM, ofs);                       /* offset label */
        for (int i = 0; i < HEX_BPR && ro + (uint32_t)i < got; i++) {
          uint32_t a = (uint32_t)(off + ro + i);
          uint8_t  b = edit_get(a, buf[ro + i]);
          char hh[3]; siprintf(hh, "%02X", b);
          int bx = 50 + i * 23;
          if (g_editing && a == g_ecur) {                 /* the editable byte: white box */
            m3_rect(bx, y, bx + 16, y + UI_ROW_H, RGB15(31, 31, 31));
            ui_text(bx, y, RGB15(0, 0, 0), hh);
          } else {
            ui_text(bx, y, edit_find(a) >= 0 ? UI_WARN : UI_SAVECLR, hh);
          }
        }
      } else {
        char t[TXT_BPR + 1];
        int c = 0;
        for (; c < bpr && ro + (uint32_t)c < got; c++) {
          uint8_t ch = buf[ro + c];
          t[c] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '.';
        }
        t[c] = 0;
        ui_text(2, y, UI_TEXT, t);
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
    ui_text(2, FOOT_Y, UI_DIM, "A hex/txt  L/R page  ST edit  B back");
  }
}

/* Page-windowed viewer: keeps a byte offset and re-reads one screen worth of
 * bytes on each move (f_lseek + f_read into an EWRAM buffer), so it handles
 * files far larger than RAM. Reads happen BEFORE render() — never mid-transfer
 * with the ROM unmapped. */
static void file_viewer(const char* path, const char* name, uint64_t size) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    show_msg("Open failed", base_name(path));
    ui_text(6, 100, UI_DIM, "B = back");
    wait_keys(KEY_B);
    return;
  }

  /* Let the d-pad + shoulders auto-repeat while in the viewer (paging and, in
   * edit mode, cursor/value changes); restored to the browser's mask on exit. */
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R);

  bool hex = true, quit = false, fopen = true;
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
            g_nedits = 0; g_editing = false;
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
  key_repeat_mask(KEY_UP | KEY_DOWN);        /* restore the browser's repeat mask */
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
  char l1[40];
  siprintf(l1, "Delete %d items?", count);
  if (!confirm(l1, "Folders include all contents")) return false;
  show_msg("Deleting...", "marked items");
  int ok = 0, fail = 0;
  for (int i = 0; i < g_n; i++) {
    if (!g_marked[i]) continue;
    char p[PATH_MAX];
    if (!path_join(g_cwd, g_entries[i].name, p)) { fail++; continue; }
    FRESULT fr = fsop_delete(p);
    log_line("batch delete %s : %d", p, fr);
    if (fr == FR_OK) ok++; else fail++;
  }
  log_flush_to_sd(LOG_PATH);
  if (fail) { char m[48]; siprintf(m, "%d ok  %d failed", ok, fail); msg_screen("Delete finished", UI_WARN, m); }
  return ok > 0;
}

/* Batch-actions menu for the marked set (selection mode, Omega-only). Returns
 * true if the listing changed. Copy/Cut capture the marked items to the
 * clipboard and leave selection mode so the user can navigate + paste. */
static bool batch_menu(int count) {
  char labels[4][24];
  int  ids[4], ni = 0;
  siprintf(labels[ni], "Copy %d", count);   ids[ni++] = 0;
  siprintf(labels[ni], "Cut %d", count);    ids[ni++] = 1;
  siprintf(labels[ni], "Delete %d", count); ids[ni++] = 2;
  strcpy(labels[ni], "Cancel");             ids[ni++] = 3;

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
        case 0:
        case 1: {
          int cap = capture_marked(ids[sel] == 0 ? CLIP_COPY : CLIP_CUT);
          if (cap < count) msg_screen("Clipboard full", UI_WARN, "Not all items captured");
          g_selmode = false;
          return false;                       /* navigate, then Paste */
        }
        case 2: { bool r = do_delete_marked(count); g_selmode = false; return r; }
        case 3: return false;
      }
    }
  }
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
    else if (hit & KEY_LEFT)  { sel -= 11; if (sel < 0) sel = 0; }                       /* jump 11 up   */
    else if (hit & KEY_RIGHT) { sel += 11; if (sel >= rows) sel = rows ? rows - 1 : 0; } /* jump 11 down */
    else if (hit & KEY_L)     { sel -= LIST_ROWS; if (sel < 0) sel = 0; }                /* page up   */
    else if (hit & KEY_R)     { sel += LIST_ROWS; if (sel >= rows) sel = rows ? rows - 1 : 0; } /* page down */
    else if (g_selmode) {
      /* selection mode: A marks the highlighted entry, START marks all/none,
       * SELECT opens batch actions, B leaves selection mode. */
      if (hit & KEY_A) {
        FsEntry* e = br_entry(sel);
        if (e) { int idx = (int)(e - g_entries); g_marked[idx] = !g_marked[idx]; }
      } else if (hit & KEY_START) {
        bool none = (marked_count() == 0);
        for (int i = 0; i < g_n; i++) g_marked[i] = none;   /* none -> all, else clear */
      } else if (hit & KEY_SELECT) {
        int mc = marked_count();
        if (mc == 0) msg_screen("No items marked", UI_DIM, "A marks the highlighted item");
        else if (batch_menu(mc)) { rescan(); sel = 0; top = 0; }
      } else if (hit & KEY_B) {
        g_selmode = false;
        memset(g_marked, 0, sizeof(g_marked));
      }
    }
    else if (hit & KEY_START) {
      /* cycle the 6 sort states: Name/Size/Date x ascending/descending */
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
        if (path_join(g_cwd, e->name, np)) { strcpy(g_cwd, np); rescan(); sel = 0; top = 0; }
      } else {
        if (actions_menu(e)) { rescan(); }        /* file -> actions menu */
      }
    }
    else if (hit & KEY_B) {
      if (!at_root()) { path_up(); rescan(); sel = 0; top = 0; }
    }
    else if (hit & KEY_SELECT) {
      /* SELECT: view a file, or open the actions menu on a folder / [..] */
      FsEntry* e = br_entry(sel);
      if (!e || e->is_dir) {
        if (actions_menu(e)) { rescan(); }
      } else {
        char np[PATH_MAX];
        if (path_join(g_cwd, e->name, np)) file_viewer(np, e->name, e->size);
      }
    }
  }
}

/* ---- bring-up ----------------------------------------------------------- */

static void init_system(void) {
  irq_init(NULL);
  irq_add(II_VBLANK, NULL);
  ui_init();
  key_repeat_mask(KEY_UP | KEY_DOWN);
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
  log_line("=== SD File Browser (sd-browser, Phase 0) ===");
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

  run_browser();
  return 0;
}
