#include "osk.h"

#include <tonc.h>
#include <string.h>

#include "ui.h"

#define OSK_ROWS   8
#define OSK_MAXLEN 200

/* QWERTY-style layout. Rows have different lengths (handled per row). Both
 * letter cases are shown directly so there is no shift mode to manage. Every
 * glyph here is legal in a FAT/exFAT long file name — the FAT-illegal set
 * (\ / : * ? " < > |) is deliberately absent (name_ok re-checks defensively).
 * The space is the first cell of the bottom row (the highlight bar shows it). */
static const char* const KB[OSK_ROWS] = {
  "1234567890",
  "qwertyuiop",
  "asdfghjkl",
  "zxcvbnm",
  "QWERTYUIOP",
  "ASDFGHJKL",
  "ZXCVBNM",
  " -_.()[]+=@~!",
};

static int rowlen(int r) { return (int)strlen(KB[r]); }

static void osk_vsync(void) { VBlankIntrWait(); key_poll(); }

static bool name_ok(const char* s) {
  int n = (int)strlen(s);
  if (n == 0) return false;
  if (s[0] == ' ' || s[n - 1] == ' ' || s[n - 1] == '.') return false;
  if (!strcmp(s, ".") || !strcmp(s, "..")) return false;
  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20) return false;
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
        c == '"'  || c == '<' || c == '>' || c == '|')
      return false;
  }
  return true;
}

/* Text field with a movable caret. Draws a horizontal window of the buffer,
 * one cell per byte, and marks the editable cell (the caret) with a WHITE box
 * and BLACK text (inverse video) — so the user can see exactly where the next
 * insert/backspace lands. The window scrolls to keep the caret on screen.
 * NOTE: one byte == one cell. The QWERTY keyboard only inserts ASCII, so this
 * is exact for typed text; a pre-filled multi-byte UTF-8 name would be edited
 * per byte (fine for the common ASCII case). */
static void osk_field(const char* buf, int len, int cpos) {
  ui_panel(4, 16, 232, 14, UI_PANEL, UI_BORDER);
  const int x0 = 8, y = 19, cols = 28;
  int scroll = (cpos > cols - 1) ? cpos - (cols - 1) : 0;   /* keep caret visible */
  for (int i = 0; i < cols; i++) {
    int ci = scroll + i;
    if (ci > len) break;
    int x = x0 + i * 8;
    char ch[2];
    ch[0] = (ci < len) ? buf[ci] : ' ';    /* caret can sit just past the last char */
    ch[1] = 0;
    if (ci == cpos) {                        /* editable cell: inverse video (theme-aware) */
      m3_rect(x, y, x + 8, y + UI_ROW_H, UI_TEXT);
      ui_text(x, y, UI_BG, ch);
    } else if (ci < len) {
      ui_text(x, y, UI_TEXT, ch);
    }
  }
}

static void osk_render(const char* prompt, const char* buf, int len, int cpos,
                       int cr, int cc, const char* warn) {
  ui_clear();
  char p[40];
  ui_truncate(p, prompt, 29);
  ui_text(2, 0, UI_TITLE, p);

  osk_field(buf, len, cpos);

  for (int r = 0; r < OSK_ROWS; r++) {
    int y = 44 + r * 13;
    int rl = rowlen(r);
    for (int c = 0; c < rl; c++) {
      int x = 8 + c * 17;
      char cell[2] = { KB[r][c], 0 };
      ui_text_sel(x, y, 13, (r == cr && c == cc), UI_TEXT, cell);
    }
  }

  char f[40];
  ui_truncate(f, warn ? warn : "A ins B del  L/R move  ST ok", 29);
  ui_text(2, 150, warn ? UI_WARN : UI_DIM, f);
}

bool osk_input(const char* prompt, const char* initial, char* out, int cap) {
  char buf[OSK_MAXLEN + 1];
  int len = 0;
  buf[0] = 0;
  if (initial) {
    for (; initial[len] && len < OSK_MAXLEN; len++) buf[len] = initial[len];
    buf[len] = 0;
  }

  int cr = 0, cc = 0;
  int cpos = len;          /* text caret: 0..len (len = just past the last char) */
  bool dirty = true;
  const char* warn = NULL;
  bool ret = false;

  /* Let the grid cursor (d-pad) and the text caret (L/R) auto-repeat on a held
   * press, using the global key-repeat timing the user set in Settings. Save the
   * caller's repeat mask and restore it exactly on exit (the viewer calls us for
   * "go to offset" mid-session and must keep its own paging-repeat mask). */
  u16 saved_mask = ui_get_repeat_mask();
  ui_set_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R);

  while (1) {
    if (cc >= rowlen(cr)) cc = rowlen(cr) - 1;   /* keep grid cursor in-row */
    if (dirty) { osk_render(prompt, buf, len, cpos, cr, cc, warn); dirty = false; }
    osk_vsync();

    /* d-pad + L/R slide (auto-repeat); insert/delete/confirm/cancel are discrete. */
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R);
    u16 hit = key_hit(KEY_A | KEY_B | KEY_START | KEY_SELECT);
    if (!mv && !hit) continue;
    dirty = true;
    warn = NULL;

    if (hit & KEY_SELECT) { ret = false; break; }
    else if (hit & KEY_START) {
      if (!name_ok(buf)) {
        warn = "Invalid name - fix & retry";
      } else if ((int)strlen(buf) >= cap - 1) {
        /* Reject rather than truncate: a chop could re-introduce a trailing
         * dot/space (which FAT strips) or split a UTF-8 codepoint. */
        warn = "Name too long";
      } else {
        int i = 0;
        for (; i < cap - 1 && buf[i]; i++) out[i] = buf[i];
        out[i] = 0;
        ret = true; break;
      }
    }
    else if (hit & KEY_A) {                  /* insert the grid char AT the caret */
      if (len < OSK_MAXLEN) {
        for (int j = len; j > cpos; j--) buf[j] = buf[j - 1];
        buf[cpos] = KB[cr][cc];
        len++; cpos++;
        buf[len] = 0;
      }
    }
    else if (hit & KEY_B) {                  /* delete the char BEFORE the caret */
      if (cpos > 0) {
        for (int j = cpos - 1; j < len; j++) buf[j] = buf[j + 1];
        len--; cpos--;
      }
    }
    else if (mv & KEY_L) { if (cpos > 0)   cpos--; }   /* caret left  */
    else if (mv & KEY_R) { if (cpos < len) cpos++; }   /* caret right */
    else if (mv & KEY_UP)    { cr = (cr == 0) ? OSK_ROWS - 1 : cr - 1; }
    else if (mv & KEY_DOWN)  { cr = (cr + 1) % OSK_ROWS; }
    else if (mv & KEY_LEFT)  { int rl = rowlen(cr); cc = (cc == 0) ? rl - 1 : cc - 1; }
    else if (mv & KEY_RIGHT) { int rl = rowlen(cr); cc = (cc + 1) % rl; }
  }

  ui_set_repeat_mask(saved_mask);            /* restore the caller's repeat mask */
  return ret;
}
