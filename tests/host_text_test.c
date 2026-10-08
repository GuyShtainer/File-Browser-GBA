/* Host tests for source/textbuf.c (pure C). Run via tests/run_host.sh. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../source/textbuf.h"

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t mem[TB_CAP];

static void load_s(const char* s) { tb_attach(mem, TB_CAP); CHECK(tb_load((const uint8_t*)s, (uint32_t)strlen(s))); }
static bool is(const char* s) { return tb_len() == strlen(s) && memcmp(tb_data(), s, tb_len()) == 0; }
static void ins(uint32_t p, const char* s) { CHECK(tb_insert(p, (const uint8_t*)s, (uint32_t)strlen(s))); }

static void t_roundtrip(void) {
  uint32_t car = 0;
  const char* orig = "hello world\nsecond line\n";
  int i;
  load_s(orig);
  ins(5, ",");
  CHECK(tb_dirty());
  ins(0, ">> ");
  CHECK(tb_delete(3, 5));
  CHECK(tb_newline(2));
  CHECK(tb_undo_count() == 4);
  for (i = 0; i < 4; i++) CHECK(tb_undo(&car));
  CHECK(is(orig));
  CHECK(!tb_undo(&car));
  /* undo caret placement */
  load_s("abcdef");
  CHECK(tb_delete(2, 2));            /* abef */
  CHECK(tb_undo(&car)); CHECK(car == 4); CHECK(is("abcdef"));
  ins(3, "XY");
  CHECK(tb_undo(&car)); CHECK(car == 3); CHECK(is("abcdef"));
  /* range refusal */
  CHECK(!tb_delete(5, 5)); CHECK(!tb_insert(7, (const uint8_t*)"x", 1));
  /* empty file */
  load_s("");
  CHECK(tb_len() == 0); CHECK(!tb_crlf());
  { TbRow r[2]; int n = tb_layout(0, 2, r); CHECK(n == 1); CHECK(r[0].endcell && r[0].ncell == 1 && r[0].next == r[0].start); }
  ins(0, "a"); CHECK(is("a")); CHECK(tb_undo(&car)); CHECK(tb_len() == 0);
  /* no trailing newline */
  load_s("abc");
  { TbRow r[2]; int n = tb_layout(0, 2, r); CHECK(n == 1); CHECK(r[0].ncell == 4 && r[0].off[3] == 3 && r[0].endcell); }
  CHECK(tb_caret_end(0) == 3);
}

static void t_merge_and_pool(void) {
  int i;
  uint32_t car;
  load_s("");
  for (i = 0; i < 20; i++) ins((uint32_t)i, "a");
  CHECK(tb_undo_count() == 1);                 /* 20 contiguous chars merged (<=32) */
  CHECK(tb_undo(&car)); CHECK(tb_len() == 0);
  /* backspace run merges and undoes whole */
  load_s("0123456789");
  CHECK(tb_delete(9, 1)); CHECK(tb_delete(8, 1)); CHECK(tb_delete(7, 1));
  CHECK(tb_undo_count() == 1); CHECK(tb_undo(&car)); CHECK(is("0123456789"));
  /* pool overflow drops oldest, newest still undoable */
  load_s("");
  for (i = 0; i < 2000; i++) ins(0, "z");      /* prepends never merge: 2000 records of 7 B */
  CHECK(tb_undo_count() < 2000); CHECK(tb_undo_count() > 400);
  CHECK(tb_len() == 2000);
  { int k = tb_undo_count(); int u = 0; while (tb_undo(&car)) u++; CHECK(u == k); CHECK(tb_len() == (uint32_t)(2000 - k)); }
  /* huge delete: undo history cleared, edit still done */
  { static uint8_t big[6000]; memset(big, 'q', sizeof big); tb_attach(mem, TB_CAP); CHECK(tb_load(big, sizeof big));
    CHECK(tb_delete(0, 5000)); CHECK(tb_len() == 1000); CHECK(tb_undo_count() == 0); }
}

static void t_crlf(void) {
  uint32_t car;
  load_s("a\r\nb\r\nc");
  CHECK(tb_crlf());
  load_s("a\nb\r\nc\n");
  CHECK(!tb_crlf());
  load_s("a\r\nb\r\n");
  CHECK(tb_newline(1)); CHECK(is("a\r\n\r\nb\r\n"));
  load_s("a\nb\n"); CHECK(tb_newline(1)); CHECK(is("a\n\nb\n"));
  load_s("a\r\nb");
  CHECK(tb_backspace_len(3) == 2);             /* caret after the pair */
  CHECK(tb_backspace_len(1) == 1);
  CHECK(tb_backspace_len(0) == 0);
  load_s("a\r\nb");
  CHECK(tb_delete(3 - tb_backspace_len(3), tb_backspace_len(3))); CHECK(is("ab"));
  CHECK(tb_undo(&car)); CHECK(is("a\r\nb")); CHECK(car == 3);
  /* caret never in the middle of a pair */
  load_s("a\r\nb");
  CHECK(tb_snap(2) == 1);
  CHECK(tb_caret_right(1) == 3);
  CHECK(tb_caret_left(3) == 1);
  CHECK(tb_caret_left(2) == 0);
  { uint32_t c; int i; for (i = 0, c = 0; i < 6; i++) { c = tb_caret_right(c); CHECK(c != 2); } CHECK(c == 4); }
  { uint32_t c = 4; int i; for (i = 0; i < 6; i++) { c = tb_caret_left(c); CHECK(c != 2); } CHECK(c == 0); }
  /* layout: CRLF is one break, end cell at the CR offset */
  { TbRow r[3]; int n = tb_layout(0, 3, r);
    CHECK(n == 2); CHECK(r[0].ncell == 2 && r[0].endcell && r[0].off[1] == 1 && r[0].next == 3);
    CHECK(r[1].start == 3 && r[1].next == 3 && r[1].ch[0] == 'b'); }
  CHECK(tb_line_of(3) == 2); CHECK(tb_col_of(3) == 1); CHECK(tb_col_of(1) == 2);
}

static void t_layout_wrap(void) {
  char line[100];
  TbRow r[8];
  int n, i;
  uint32_t caret;
  /* hard wrap of one long word at TB_COLS */
  memset(line, 'x', 70); line[70] = 0;
  load_s(line);
  n = tb_layout(0, 8, r);
  CHECK(n == 3);
  CHECK(r[0].ncell == TB_COLS && !r[0].endcell && r[0].next == TB_COLS);
  CHECK(r[1].start == TB_COLS && r[1].next == 2 * TB_COLS);
  CHECK(r[2].start == 2 * TB_COLS && r[2].endcell && r[2].next == r[2].start && r[2].ncell == 70 - 2 * TB_COLS + 1);
  /* every caret offset maps to exactly one cell, and cells map back */
  for (caret = 0; caret <= 70; caret++) {
    uint32_t rs = tb_row_start_of(caret);
    int ri = -1, ci = -1;
    for (i = 0; i < n; i++) if (r[i].start == rs) ri = i;
    CHECK(ri >= 0);
    if (ri >= 0) { for (i = 0; i < r[ri].ncell; i++) if (r[ri].off[i] == caret) { ci = i; break; } CHECK(ci >= 0); }
  }
  /* exactly TB_COLS chars then newline: end cell is cell index TB_COLS */
  memset(line, 'y', TB_COLS); line[TB_COLS] = '\n'; line[TB_COLS + 1] = 'z'; line[TB_COLS + 2] = 0;
  load_s(line);
  n = tb_layout(0, 8, r);
  CHECK(n == 2); CHECK(r[0].ncell == TB_COLS + 1 && r[0].endcell && r[0].off[TB_COLS] == TB_COLS);
  CHECK(r[1].start == TB_COLS + 1);
  /* word wrap: break after a space when the word does not fit */
  memset(line, 'a', 25); line[25] = ' '; memset(line + 26, 'b', 10); line[36] = 0;
  load_s(line);
  n = tb_layout(0, 8, r);
  CHECK(n == 2); CHECK(r[0].ncell == 26 && r[0].next == 26 && r[1].start == 26);
  /* wrap-row navigation */
  CHECK(tb_row_start_of(30) == 26);
  CHECK(tb_prev_row_start(26) == 0);
  CHECK(tb_caret_down(3) == 26 + 3);
  CHECK(tb_caret_up(26 + 3) == 3);
  CHECK(tb_caret_home(30) == 26);
  /* up/down across lines of different length, CRLF included */
  load_s("longer line\r\nab\r\nlonger again");
  CHECK(tb_caret_down(8) == 13 + 2);           /* clamps to ab's end cell (CR offset 15) */
  CHECK(tb_caret_down(15) == 17 + 2);          /* col 2 on third line */
  CHECK(tb_caret_up(19) == 15);
  CHECK(tb_caret_up(15) == 2);
  CHECK(tb_caret_up(2) == 2);                  /* top row */
  CHECK(tb_caret_down(tb_len()) == tb_len());  /* last row */
}

static void t_tabs_binary(void) {
  TbRow r[2];
  uint8_t b[] = { 'a', '\t', 'b', 0x00, 0xC3, 0xA9, 'z', 0x7F };
  load_s("a\tb");
  tb_layout(0, 2, r);
  CHECK(r[0].ncell == 6);                       /* a + 3 tab cells + b + end */
  CHECK(r[0].off[1] == 1 && r[0].off[2] == 1 && r[0].off[3] == 1 && r[0].off[4] == 2 && r[0].off[5] == 3);
  CHECK(r[0].ch[1] == ' ' && r[0].ch[4] == 'b');
  load_s("\tx");
  tb_layout(0, 2, r);
  CHECK(r[0].off[3] == 0 && r[0].off[4] == 1);
  /* binary: shown as '.', stored untouched after edits elsewhere */
  tb_attach(mem, TB_CAP);
  CHECK(tb_load(b, sizeof b));
  CHECK(tb_has_binary());
  tb_layout(0, 2, r);
  CHECK(r[0].ch[5] == '.' && r[0].ch[6] == '.' && r[0].ch[7] == '.' && r[0].ch[9] == '.');
  ins(0, "Q"); CHECK(tb_delete(1, 1));          /* "Q" + original minus 'a' */
  CHECK(tb_len() == sizeof b);
  CHECK(memcmp(tb_data() + 2, b + 2, sizeof b - 2) == 0);
  load_s("plain ascii"); CHECK(!tb_has_binary());
}

static void t_cap(void) {
  static uint8_t big[TB_CAP + 1];
  static uint8_t small[16];
  uint32_t car;
  memset(big, 'k', sizeof big);
  tb_attach(mem, TB_CAP);
  CHECK(!tb_load(big, TB_CAP + 1));             /* over cap refused, buffer empty */
  CHECK(tb_len() == 0);
  CHECK(tb_load(big, TB_CAP));
  CHECK(!tb_insert(0, (const uint8_t*)"x", 1)); /* full: insert refused, unchanged */
  CHECK(tb_len() == TB_CAP); CHECK(!tb_dirty()); CHECK(tb_undo_count() == 0);
  CHECK(!tb_newline(0));
  CHECK(tb_delete(0, 1)); CHECK(tb_insert(0, (const uint8_t*)"x", 1));
  /* smaller attach cap */
  tb_attach(small, sizeof small);
  CHECK(!tb_load(big, 17)); CHECK(tb_load(big, 16)); CHECK(!tb_insert(0, (const uint8_t*)"x", 1));
  CHECK(tb_undo(&car) == false);
  /* clean flag */
  load_s("x"); ins(1, "y"); CHECK(tb_dirty()); tb_clean(); CHECK(!tb_dirty());
}

int main(void) {
  t_roundtrip(); t_merge_and_pool(); t_crlf(); t_layout_wrap(); t_tabs_binary(); t_cap();
  printf("host_text_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
