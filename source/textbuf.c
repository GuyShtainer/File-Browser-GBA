/* textbuf.c - pure-C text-editing core (see textbuf.h). No tonc/GBA headers. */
#include "textbuf.h"
#include <string.h>

/* The 4 KiB undo pool must not eat the 32 KiB IWRAM: on the GBA build it goes to
 * EWRAM (.sbss, same trick as fs_ops.c); host builds keep an ordinary static. */
#if defined(__arm__)
#define TB_EWRAM __attribute__((section(".sbss")))
#else
#define TB_EWRAM
#endif

#define UNDO_POOL  4096u
#define UNDO_HDR   7u
#define UNDO_MERGE 32u          /* longest typed/deleted run merged into one record */
#define OP_INS     1u
#define OP_DEL     2u

static uint8_t* s_mem;
static uint32_t s_cap;
static uint32_t s_len;
static bool     s_dirty;
static bool     s_crlf;

static uint8_t  TB_EWRAM s_undo[UNDO_POOL];
static uint32_t s_ulen;         /* bytes used in the pool            */
static uint32_t s_ulast;        /* offset of the newest record       */
static int      s_ucount;

/* ---- undo pool ----------------------------------------------------------- */

static uint32_t rec_n(uint32_t o)   { return (uint32_t)s_undo[o + 1] | ((uint32_t)s_undo[o + 2] << 8); }
static uint32_t rec_pos(uint32_t o) {
  return (uint32_t)s_undo[o + 3] | ((uint32_t)s_undo[o + 4] << 8) |
         ((uint32_t)s_undo[o + 5] << 16) | ((uint32_t)s_undo[o + 6] << 24);
}
static uint32_t rec_size(uint32_t o) {
  return UNDO_HDR + (s_undo[o] == OP_DEL ? rec_n(o) : 0u);
}
static void rec_put(uint32_t o, uint32_t op, uint32_t pos, uint32_t n) {
  s_undo[o] = (uint8_t)op;
  s_undo[o + 1] = (uint8_t)(n & 0xFFu);
  s_undo[o + 2] = (uint8_t)((n >> 8) & 0xFFu);
  s_undo[o + 3] = (uint8_t)(pos & 0xFFu);
  s_undo[o + 4] = (uint8_t)((pos >> 8) & 0xFFu);
  s_undo[o + 5] = (uint8_t)((pos >> 16) & 0xFFu);
  s_undo[o + 6] = (uint8_t)((pos >> 24) & 0xFFu);
}

static void undo_clear(void) { s_ulen = 0; s_ulast = 0; s_ucount = 0; }

/* Recompute s_ulast by walking the (<= ~585) records. */
static void undo_find_last(void) {
  uint32_t o = 0, last = 0;
  int i;
  for (i = 0; i < s_ucount; i++) {
    last = o;
    o += rec_size(o);
  }
  s_ulast = last;
}

static void undo_drop_oldest(void) {
  uint32_t sz;
  if (s_ucount <= 0) { undo_clear(); return; }
  sz = rec_size(0);
  memmove(s_undo, s_undo + sz, s_ulen - sz);
  s_ulen -= sz;
  s_ucount--;
  if (s_ucount == 0) undo_clear(); else undo_find_last();
}

static void undo_record(uint32_t op, uint32_t pos, const uint8_t* data, uint32_t n) {
  uint32_t need, o;
  if (n == 0 || n > 0xFFFFu) { undo_clear(); return; }
  need = UNDO_HDR + (op == OP_DEL ? n : 0u);
  if (need > UNDO_POOL) { undo_clear(); return; }   /* too big to undo: history lost */
  if (s_ucount > 0) {
    o = s_ulast;
    if (op == OP_INS && s_undo[o] == OP_INS && rec_pos(o) + rec_n(o) == pos &&
        rec_n(o) + n <= UNDO_MERGE) {
      rec_put(o, OP_INS, rec_pos(o), rec_n(o) + n);
      return;
    }
    if (op == OP_DEL && s_undo[o] == OP_DEL && rec_n(o) + n <= UNDO_MERGE &&
        s_ulen + n <= UNDO_POOL) {
      if (rec_pos(o) == pos) {                       /* forward delete: append */
        memcpy(s_undo + s_ulen, data, n);
        rec_put(o, OP_DEL, pos, rec_n(o) + n);
        s_ulen += n;
        return;
      }
      if (pos + n == rec_pos(o)) {                   /* backspace run: prepend */
        memmove(s_undo + o + UNDO_HDR + n, s_undo + o + UNDO_HDR, rec_n(o));
        memcpy(s_undo + o + UNDO_HDR, data, n);
        rec_put(o, OP_DEL, pos, rec_n(o) + n);
        s_ulen += n;
        return;
      }
    }
  }
  while (s_ucount > 0 && s_ulen + need > UNDO_POOL) undo_drop_oldest();
  o = s_ulen;
  rec_put(o, op, pos, n);
  if (op == OP_DEL) memcpy(s_undo + o + UNDO_HDR, data, n);
  s_ulen += need;
  s_ulast = o;
  s_ucount++;
}

/* ---- buffer -------------------------------------------------------------- */

void tb_attach(uint8_t* mem, uint32_t cap) {
  s_mem = mem;
  s_cap = (mem == NULL) ? 0u : (cap > TB_CAP ? TB_CAP : cap);
  s_len = 0;
  s_dirty = false;
  s_crlf = false;
  undo_clear();
}

bool tb_load(const uint8_t* data, uint32_t n) {
  uint32_t i, crlf = 0, lf = 0;
  s_len = 0;
  s_dirty = false;
  s_crlf = false;
  undo_clear();
  if (s_mem == NULL || n > s_cap) return false;
  if (n > 0) {
    if (data == NULL) return false;
    memmove(s_mem, data, n);          /* memmove: data may already be the buffer */
  }
  s_len = n;
  for (i = 0; i < n; i++) {
    if (s_mem[i] == '\n') {
      if (i > 0 && s_mem[i - 1] == '\r') crlf++; else lf++;
    }
  }
  s_crlf = (crlf > 0 && crlf > lf);
  return true;
}

uint32_t       tb_len(void)  { return s_len; }
const uint8_t* tb_data(void) { return s_mem; }
uint8_t        tb_at(uint32_t i) { return (s_mem != NULL && i < s_len) ? s_mem[i] : 0u; }
bool           tb_dirty(void) { return s_dirty; }
void           tb_clean(void) { s_dirty = false; }
bool           tb_crlf(void)  { return s_crlf; }
int            tb_undo_count(void) { return s_ucount; }

static bool raw_insert(uint32_t pos, const uint8_t* s, uint32_t n) {
  if (s_mem == NULL || pos > s_len || n > s_cap - s_len) return false;
  if (n == 0) return true;
  if (s == NULL) return false;
  memmove(s_mem + pos + n, s_mem + pos, s_len - pos);
  memcpy(s_mem + pos, s, n);
  s_len += n;
  return true;
}

static bool raw_delete(uint32_t pos, uint32_t n) {
  if (s_mem == NULL || pos > s_len || n > s_len - pos) return false;
  memmove(s_mem + pos, s_mem + pos + n, s_len - pos - n);
  s_len -= n;
  return true;
}

bool tb_insert(uint32_t pos, const uint8_t* s, uint32_t n) {
  if (!raw_insert(pos, s, n)) return false;
  if (n > 0) { undo_record(OP_INS, pos, NULL, n); s_dirty = true; }
  return true;
}

bool tb_delete(uint32_t pos, uint32_t n) {
  if (s_mem == NULL || pos > s_len || n > s_len - pos) return false;
  if (n == 0) return true;
  undo_record(OP_DEL, pos, s_mem + pos, n);   /* copy bytes BEFORE removing */
  (void)raw_delete(pos, n);                   /* range checked above */
  s_dirty = true;
  return true;
}

bool tb_newline(uint32_t pos) {
  static const uint8_t crlf[2] = { '\r', '\n' };
  static const uint8_t lf[1] = { '\n' };
  return s_crlf ? tb_insert(pos, crlf, 2) : tb_insert(pos, lf, 1);
}

bool tb_undo(uint32_t* out_caret) {
  uint32_t o, pos, n;
  if (s_ucount <= 0) return false;
  o = s_ulast;
  pos = rec_pos(o);
  n = rec_n(o);
  if (s_undo[o] == OP_INS) {
    if (!raw_delete(pos, n)) return false;
    if (out_caret) *out_caret = tb_snap(pos);
  } else {
    if (!raw_insert(pos, s_undo + o + UNDO_HDR, n)) return false;
    if (out_caret) *out_caret = tb_snap(pos + n);
  }
  s_ulen = o;
  s_ucount--;
  if (s_ucount == 0) undo_clear(); else undo_find_last();
  s_dirty = true;
  return true;
}

/* ---- layout -------------------------------------------------------------- */

static bool is_break_at(uint32_t pos) {
  if (pos >= s_len) return false;
  if (s_mem[pos] == '\n') return true;
  return s_mem[pos] == '\r' && pos + 1 < s_len && s_mem[pos + 1] == '\n';
}

static void put_cell(TbRow* r, int n, uint32_t off, uint8_t g) {
  r->off[n] = off;
  r->ch[n] = g;
}

static uint8_t glyph(uint8_t c) { return (c >= 0x20 && c <= 0x7E) ? c : (uint8_t)'.'; }

static void build_row(uint32_t start, TbRow* r) {
  uint32_t pos = start, brk_pos = 0;
  int n = 0, brk_n = 0, guard;
  r->start = start;
  r->next = start;
  r->endcell = 0;
  for (guard = 0; guard < TB_COLS + 4; guard++) {
    uint8_t c;
    if (pos >= s_len) {                         /* past-the-end caret cell: last row */
      put_cell(r, n, s_len, ' ');
      n++;
      r->endcell = 1;
      r->next = start;
      break;
    }
    if (is_break_at(pos)) {                     /* end-of-line caret cell */
      put_cell(r, n, pos, ' ');
      n++;
      r->endcell = 1;
      r->next = pos + (s_mem[pos] == '\r' ? 2u : 1u);
      break;
    }
    c = s_mem[pos];
    if (n >= TB_COLS) {                         /* wrap */
      if (brk_n > 0 && c != ' ' && c != '\t') { n = brk_n; r->next = brk_pos; }
      else r->next = pos;
      break;
    }
    if (c == '\t') {
      int w = 4 - (n % 4);
      while (w > 0 && n < TB_COLS) { put_cell(r, n, pos, ' '); n++; w--; }
    } else {
      put_cell(r, n, pos, glyph(c));
      n++;
    }
    pos++;
    if (c == ' ' || c == '\t') { brk_n = n; brk_pos = pos; }
  }
  r->ncell = (uint8_t)n;
}

int tb_layout(uint32_t top, int nrows, TbRow* rows) {
  uint32_t pos = top;
  int i;
  if (rows == NULL || nrows <= 0) return 0;
  if (pos > s_len) pos = s_len;
  for (i = 0; i < nrows; i++) {
    build_row(pos, &rows[i]);
    if (rows[i].next == pos) return i + 1;      /* last row of the buffer */
    pos = rows[i].next;
  }
  return nrows;
}

static uint32_t line_start(uint32_t p) {
  if (p > s_len) p = s_len;
  while (p > 0 && s_mem[p - 1] != '\n') p--;
  return p;
}

uint32_t tb_row_start_of(uint32_t off) {
  TbRow r;
  uint32_t s;
  if (off > s_len) off = s_len;
  s = line_start(off);
  for (;;) {                                    /* bounded: each row advances >= 1 byte */
    build_row(s, &r);
    if (r.next == s || off < r.next) return s;
    s = r.next;
  }
}

uint32_t tb_prev_row_start(uint32_t off) {
  TbRow r;
  uint32_t s;
  if (off > s_len) off = s_len;
  if (off == 0) return 0;
  s = line_start(off - 1);
  for (;;) {
    build_row(s, &r);
    if (r.next == s || r.next >= off) return s;
    s = r.next;
  }
}

uint32_t tb_snap(uint32_t c) {
  if (c > s_len) c = s_len;
  if (c > 0 && c < s_len && s_mem[c] == '\n' && s_mem[c - 1] == '\r') c--;
  return c;
}

uint32_t tb_caret_left(uint32_t caret) {
  uint32_t c = tb_snap(caret);
  return c == 0 ? 0u : tb_snap(c - 1);
}

uint32_t tb_caret_right(uint32_t caret) {
  uint32_t c = tb_snap(caret);
  if (c >= s_len) return s_len;
  if (is_break_at(c) && s_mem[c] == '\r') return c + 2;
  return c + 1;
}

static int cell_of(const TbRow* r, uint32_t caret) {
  int i;
  for (i = 0; i < r->ncell; i++) if (r->off[i] >= caret) return i;
  return r->ncell > 0 ? r->ncell - 1 : 0;
}

static uint32_t at_col(const TbRow* r, int col) {
  int i = col;
  if (i >= r->ncell) i = r->ncell - 1;
  if (i < 0) return r->start;
  return tb_snap(r->off[i]);
}

uint32_t tb_caret_up(uint32_t caret) {
  TbRow r;
  uint32_t c = tb_snap(caret), rs = tb_row_start_of(c);
  int col;
  if (rs == 0) return c;
  build_row(rs, &r);
  col = cell_of(&r, c);
  build_row(tb_prev_row_start(rs), &r);
  return at_col(&r, col);
}

uint32_t tb_caret_down(uint32_t caret) {
  TbRow r;
  uint32_t c = tb_snap(caret), rs = tb_row_start_of(c);
  int col;
  build_row(rs, &r);
  if (r.next == rs) return c;                   /* already on the last row */
  col = cell_of(&r, c);
  build_row(r.next, &r);
  return at_col(&r, col);
}

uint32_t tb_caret_home(uint32_t caret) { return tb_row_start_of(tb_snap(caret)); }

uint32_t tb_caret_end(uint32_t caret) {
  TbRow r;
  build_row(tb_row_start_of(tb_snap(caret)), &r);
  return at_col(&r, r.ncell - 1);
}

uint32_t tb_backspace_len(uint32_t caret) {
  if (caret > s_len) caret = s_len;
  if (caret == 0) return 0;
  if (caret >= 2 && s_mem[caret - 1] == '\n' && s_mem[caret - 2] == '\r') return 2;
  return 1;
}

uint32_t tb_line_of(uint32_t caret) {
  uint32_t i, n = 1;
  if (caret > s_len) caret = s_len;
  for (i = 0; i < caret; i++) if (s_mem[i] == '\n') n++;
  return n;
}

uint32_t tb_col_of(uint32_t caret) {
  if (caret > s_len) caret = s_len;
  return caret - line_start(caret) + 1u;
}

bool tb_has_binary(void) {
  uint32_t i;
  for (i = 0; i < s_len; i++) if (s_mem[i] == 0 || s_mem[i] >= 0x80) return true;
  return false;
}
