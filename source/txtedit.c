#include "txtedit.h"

#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "log.h"
#include "fs_ops.h"
#include "textbuf.h"
#include "ui.h"

#define X0        2          /* left edge of the text grid (px)           */
#define Y0        10         /* top of the first text row (px)            */
#define STATUS_Y  150
#define KB_ROWS   5          /* 4 character rows + the Space/Enter/Tab row */
#define KB_Y      98         /* top of the keyboard panel                 */
#define KB_PITCH  11
#define TYPE_ROWS 10         /* text rows visible while the keyboard is up */
#define KEY_BOTTOM 3         /* number of special keys (Space Enter Tab)   */
#define PAGES     3

static const char* const KB_PAGE[PAGES][4] = {
  { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-" },
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM?!:" },
  { "!\"#$%&'()", "*+,-./:;<", "=>?@[\\]^_", "`{|}~" },
};
static const char* const KB_NAME[PAGES] = { "abc", "ABC", "sym" };

static TbRow EWRAM_BSS s_rows[TB_MAXROWS];

static uint32_t s_caret, s_top;
static bool     s_kb;
static bool     s_kb_armed;   /* false until A is released after opening the keyboard */
static int      s_page, s_kr, s_kc;
static const char* s_note;
static bool     s_saved_any;

static void tx_vsync(void) { VBlankIntrWait(); key_poll(); }

/* ---- small modal helpers (osk/main statics are not reachable from here) --- */

static int tx_pick(const char* title, const char* l1, const char* l2,
                   const char* const* items, int n) {
  int sel = 0;
  bool dirty = true;
  for (;;) {
    if (dirty) {
      ui_clear();
      if (l1) ui_text(6, 8, UI_WARN, l1);
      if (l2) ui_text(6, 20, UI_TEXT, l2);
      ui_text(6, 34, UI_TITLE, title);
      for (int i = 0; i < n; i++) ui_text_sel(6, 52 + i * 12, 200, i == sel, UI_TEXT, items[i]);
      ui_text(6, 130, UI_DIM, "A = choose   B = back");
      dirty = false;
    }
    tx_vsync();
    u16 hit = key_hit(KEY_A | KEY_B | KEY_UP | KEY_DOWN);
    if (!hit) continue;
    dirty = true;
    if (hit & KEY_B) return -1;
    if (hit & KEY_A) return sel;
    if (hit & KEY_DOWN) sel = (sel + 1) % n;
    if (hit & KEY_UP)   sel = (sel == 0) ? n - 1 : sel - 1;
  }
}

static bool tx_confirm(const char* l1, const char* l2) {
  const char* items[2] = { "Yes", "No" };
  return tx_pick("Confirm", l1, l2, items, 2) == 0;
}

static void tx_msg(const char* l1, const char* l2, const char* l3) {
  ui_clear();
  ui_text(6, 40, UI_WARN, l1);
  if (l2) ui_text(6, 54, UI_TEXT, l2);
  if (l3) ui_text(6, 68, UI_TEXT, l3);
  ui_text(6, 110, UI_DIM, "B = back");
  do { tx_vsync(); } while (!key_hit(KEY_B));
}

/* ---- load / save --------------------------------------------------------- */

static bool tx_load_file(const char* path, uint8_t* mem, uint32_t cap) {
  FIL f;
  UINT br = 0;
  uint32_t n = 0;
  FRESULT fr = f_open(&f, path, FA_READ);
  if (fr != FR_OK) { tx_msg("Cannot open file", NULL, NULL); return false; }
  FSIZE_t sz = f_size(&f);
  if (sz > cap) { f_close(&f); tx_msg("File too big to edit", NULL, NULL); return false; }
  while (n < (uint32_t)sz) {
    uint32_t want = (uint32_t)sz - n;
    if (want > 4096u) want = 4096u;
    fr = f_read(&f, mem + n, want, &br);
    if (fr != FR_OK || br == 0) break;
    n += br;
  }
  f_close(&f);
  if (n != (uint32_t)sz) { tx_msg("Read failed", NULL, NULL); return false; }
  return tb_load(mem, n);
}

static void tx_save_error(FRESULT fr, const char* path, const char* name) {
  char l[40], t[64];
  if (fr == FSOP_ERR_LEFTOVER) log_line("txtedit: refused leftover temp %s", path);
  else if (fr == FSOP_ERR_SHARED) log_line("txtedit: refused shared chain %s", path);
  else log_line("txtedit: save failed fr=%d %s", (int)fr, path);
  (void)log_flush_to_sd(LOG_PATH);   /* the outcome line reaches the card now (chain oracle) */
  if (fr == FSOP_ERR_LEFTOVER) {
    siprintf(t, "%s.txtnew~", name);
    ui_truncate(l, t, 29);
    tx_msg("Leftover temp file", l, "check it first");
  } else if (fr == FSOP_ERR_SHARED) {
    tx_msg("Save refused", "backup shares data with file", "original untouched");
  } else {
    FILINFO c;
    char tp[FS_PATH_CAP];
    bool tmp_left = false;
    if (strlen(path) + 10 < FS_PATH_CAP) {
      siprintf(tp, "%s.txtnew~", path);
      tmp_left = (f_stat(tp, &c) == FR_OK);
    }
    if (f_stat(path, &c) != FR_OK && tmp_left) {
      /* both renames failed: the data lives only in name.bak~ + name.txtnew~
         (a NEW file whose rename failed has its temp dropped: no RECOVER) */
      tx_msg("Save failed - RECOVER", "see name.bak~ and", "name.txtnew~");
    } else {
      siprintf(l, "FatFs error %d", (int)fr);
      tx_msg("Save failed", l, "buffer is still unsaved");
    }
  }
}

/* True when the file was written (buffer marked clean). */
static bool tx_save(const char* path, const char* name) {
  FRESULT fr = fsop_save_buffer(path, tb_data(), tb_len(), true);
  if (fr != FR_OK) { tx_save_error(fr, path, name); return false; }
  tb_clean();
  log_line("txtedit: saved %s", path);
  (void)log_flush_to_sd(LOG_PATH);   /* the outcome line reaches the card now (chain oracle) */
  s_saved_any = true;
  s_note = "Saved - previous kept as .bak~";
  return true;
}

/* ---- view / caret -------------------------------------------------------- */

static int tx_rows(void) { return s_kb ? TYPE_ROWS : TB_MAXROWS; }

/* Keep `s_top` a valid row start with the caret's row on screen. */
static void tx_scroll(void) {
  uint32_t crs = tb_row_start_of(s_caret);
  int rows = tx_rows();
  s_top = tb_row_start_of(s_top);
  if (crs < s_top) { s_top = crs; return; }
  int n = tb_layout(s_top, rows, s_rows);
  for (int i = 0; i < n; i++) if (s_rows[i].start == crs) return;   /* visible */
  s_top = crs;                                                    /* put caret row at the bottom */
  for (int i = 0; i < rows - 1 && s_top > 0; i++) s_top = tb_prev_row_start(s_top);
}

static void tx_draw_caret(int row, const TbRow* r) {
  for (int c = 0; c < r->ncell; c++) {
    if (r->off[c] != s_caret) continue;
    int x = X0 + c * 8, y = Y0 + row * 8;
    int x1 = (x + 8 > UI_SCR_W) ? UI_SCR_W : x + 8;
    char ch[2] = { (char)r->ch[c], 0 };
    m3_rect(x, y, x1, y + 8, UI_TEXT);
    if (x + 8 <= UI_SCR_W) ui_text(x, y, UI_BG, ch);
    return;
  }
}

static void tx_draw_text(void) {
  int n = tb_layout(s_top, tx_rows(), s_rows);
  uint32_t crs = tb_row_start_of(s_caret);
  for (int i = 0; i < n; i++) {
    char line[TB_COLS + 2];
    int len = s_rows[i].ncell - (s_rows[i].endcell ? 1 : 0);
    for (int c = 0; c < len; c++) line[c] = (char)s_rows[i].ch[c];
    line[len] = 0;
    ui_text(X0, Y0 + i * 8, UI_TEXT, line);
    if (s_rows[i].start == crs) tx_draw_caret(i, &s_rows[i]);
  }
}

/* ---- keyboard ------------------------------------------------------------ */

static int kb_rowlen(int r) {
  return (r < 4) ? (int)strlen(KB_PAGE[s_page][r]) : KEY_BOTTOM;
}

static void tx_draw_kb(void) {
  static const char* const sp[KEY_BOTTOM] = { "Space", "Enter", "Tab" };
  static const int spx[KEY_BOTTOM] = { 8, 60, 112 };
  static const int spw[KEY_BOTTOM] = { 44, 44, 26 };
  ui_panel(0, KB_Y - 3, 240, KB_ROWS * KB_PITCH + 8, UI_PANEL, UI_BORDER);
  for (int r = 0; r < 4; r++) {
    int rl = kb_rowlen(r);
    for (int c = 0; c < rl; c++) {
      char cell[2] = { KB_PAGE[s_page][r][c], 0 };
      ui_text_sel(8 + c * 20, KB_Y + r * KB_PITCH, 13, r == s_kr && c == s_kc, UI_TEXT, cell);
    }
  }
  for (int c = 0; c < KEY_BOTTOM; c++)
    ui_text_sel(spx[c], KB_Y + 4 * KB_PITCH, spw[c], s_kr == 4 && s_kc == c, UI_TEXT, sp[c]);
  ui_text(150, KB_Y + 4 * KB_PITCH, UI_DIM, "SEL:page");
  ui_text(202, KB_Y + 4 * KB_PITCH, UI_TITLE, KB_NAME[s_page]);
}

static void tx_type(uint8_t c) {
  if (tb_insert(s_caret, &c, 1)) s_caret = tb_snap(s_caret + 1);
  else s_note = "Buffer full (32 KiB)";
}

static void tx_press_key(void) {
  if (s_kr < 4) { tx_type((uint8_t)KB_PAGE[s_page][s_kr][s_kc]); return; }
  if (s_kc == 0) tx_type(' ');
  else if (s_kc == 1) {
    uint32_t add = tb_crlf() ? 2u : 1u;
    if (tb_newline(s_caret)) s_caret = tb_snap(s_caret + add);
    else s_note = "Buffer full (32 KiB)";
  } else tx_type('\t');
}

static void tx_backspace(void) {
  uint32_t n = tb_backspace_len(s_caret);
  if (n && tb_delete(s_caret - n, n)) s_caret -= n;
}

/* Returns after handling one frame's input in Type mode. */
static void tx_input_type(u16 mv, u16 hit) {
  /* the A that opened the keyboard must not auto-repeat into a typed key */
  if (!s_kb_armed) {
    /* a fresh press (hit) or a released A arms; a still-held opening A is masked.
       The release frame never reaches here (no mv/hit), so hit must count too. */
    if ((hit & KEY_A) || !key_is_down(KEY_A)) s_kb_armed = true; else mv &= (u16)~KEY_A;
  }
  if (hit & KEY_START) { s_kb = false; return; }
  /* Clamp the column on a page switch: repro was abc page, top row, LEFT (col 9),
     SELECT, SELECT, A - the shorter row on the new page was indexed past its end. */
  if (hit & KEY_SELECT) {
    s_page = (s_page + 1) % PAGES;
    if (s_kc >= kb_rowlen(s_kr)) s_kc = kb_rowlen(s_kr) - 1;
    return;
  }
  if (mv & KEY_A) { tx_press_key(); return; }
  if (mv & KEY_B) { tx_backspace(); return; }
  if (mv & KEY_L) { s_caret = tb_caret_left(s_caret); return; }
  if (mv & KEY_R) { s_caret = tb_caret_right(s_caret); return; }
  if (mv & KEY_UP)   s_kr = (s_kr == 0) ? KB_ROWS - 1 : s_kr - 1;
  if (mv & KEY_DOWN) s_kr = (s_kr + 1) % KB_ROWS;
  if (mv & KEY_LEFT) {
    int rl = kb_rowlen(s_kr);
    s_kc = (s_kc == 0) ? rl - 1 : s_kc - 1;
  }
  if (mv & KEY_RIGHT) s_kc = (s_kc + 1) % kb_rowlen(s_kr);
  if (s_kc >= kb_rowlen(s_kr)) s_kc = kb_rowlen(s_kr) - 1;
}

/* ---- navigate mode + menus ----------------------------------------------- */

static void tx_undo(void) {
  uint32_t c;
  if (tb_undo(&c)) s_caret = c; else s_note = "Nothing to undo";
}

static void tx_page(bool down) {
  for (int i = 0; i < TB_MAXROWS - 1; i++)
    s_caret = down ? tb_caret_down(s_caret) : tb_caret_up(s_caret);
}

/* Exit prompt. Returns true when the editor should close. */
static bool tx_try_exit(const char* path, const char* name) {
  if (!tb_dirty()) return true;
  const char* items[3] = { "Save and exit", "Exit without saving", "Cancel" };
  int c = tx_pick("Unsaved changes", NULL, NULL, items, 3);
  if (c == 0) return tx_save(path, name);
  if (c == 1) return true;
  return false;
}

/* START menu. Returns true when the editor should close. */
static bool tx_menu(const char* path, const char* name) {
  const char* items[3] = { "Save", "Undo", "Exit" };
  int c = tx_pick("Text editor", NULL, NULL, items, 3);
  if (c == 0) { (void)tx_save(path, name); return false; }
  if (c == 1) { tx_undo(); return false; }
  if (c == 2) return tx_try_exit(path, name);
  return false;
}

/* Returns true when the editor should close. */
static bool tx_input_nav(u16 mv, u16 hit, const char* path, const char* name) {
  if (hit & KEY_B)      return tx_try_exit(path, name);
  if (hit & KEY_START)  return tx_menu(path, name);
  if (hit & KEY_SELECT) { tx_undo(); return false; }
  if (hit & KEY_A)      { s_kb = true; s_kb_armed = false; return false; }
  if (mv & KEY_L)       { tx_page(false); return false; }
  if (mv & KEY_R)       { tx_page(true); return false; }
  if (mv & KEY_UP)      s_caret = tb_caret_up(s_caret);
  if (mv & KEY_DOWN)    s_caret = tb_caret_down(s_caret);
  if (mv & KEY_LEFT)    s_caret = tb_caret_left(s_caret);
  if (mv & KEY_RIGHT)   s_caret = tb_caret_right(s_caret);
  return false;
}

static void tx_render(const char* name) {
  char hdr[48], nb[48], st[64];
  ui_clear();
  ui_truncate(nb, name, s_kb ? 9 : 7);
  siprintf(hdr, s_kb ? "%s  A=key B=del ST=done" : "%s A=kbd ST=menu SE=undo", nb);
  ui_truncate(hdr, hdr, 29);
  ui_text(2, 0, UI_TITLE, hdr);
  tx_draw_text();
  if (s_kb) tx_draw_kb();
  if (s_note) {
    ui_truncate(st, s_note, 29);
  } else {
    siprintf(st, "Ln %lu Col %lu%s undo:%d %s", (unsigned long)tb_line_of(s_caret),
             (unsigned long)tb_col_of(s_caret), tb_dirty() ? " *" : "",
             tb_undo_count(), tb_crlf() ? "CRLF" : "LF");
    ui_truncate(st, st, 29);
  }
  ui_text(2, STATUS_Y, s_note ? UI_OK : UI_DIM, st);
}

bool txtedit_run(const char* path, const char* name, uint8_t* mem, uint32_t cap) {
  if (path == NULL || name == NULL || mem == NULL) return false;
  tb_attach(mem, cap);
  if (!tx_load_file(path, mem, cap)) return false;
  if (tb_has_binary() &&
      !tx_confirm("Binary bytes shown as '.',", "kept as-is. Edit anyway?")) return false;

  s_caret = 0; s_top = 0; s_kb = false; s_page = 0; s_kr = 0; s_kc = 0;
  s_note = NULL; s_saved_any = false;

  u16 saved_mask = ui_get_repeat_mask();
  ui_set_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B);
  bool done = false, dirty = true;
  while (!done) {
    if (dirty) {
      s_caret = tb_snap(s_caret);
      tx_scroll();
      tx_render(name);
      dirty = false;
    }
    tx_vsync();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B);
    u16 hit = key_hit(KEY_A | KEY_B | KEY_START | KEY_SELECT);
    if (!mv && !hit) continue;
    s_note = NULL;
    dirty = true;
    if (s_kb) tx_input_type(mv, hit);
    else done = tx_input_nav(mv, hit, path, name);
  }
  ui_set_repeat_mask(saved_mask);
  return s_saved_any;
}
