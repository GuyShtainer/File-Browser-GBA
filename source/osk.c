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

static void osk_field(const char* buf) {
  char shown[40];
  int len = (int)strlen(buf);
  if (len <= 28) {
    strcpy(shown, buf);
  } else {                       /* show the tail so the caret stays visible */
    shown[0] = '~';
    memcpy(shown + 1, buf + len - 27, 27);
    shown[28] = 0;
  }
  ui_panel(4, 16, 232, 14, UI_PANEL, UI_BORDER);
  ui_text(8, 19, UI_TEXT, len ? shown : "(empty)");
}

static void osk_render(const char* prompt, const char* buf, int cr, int cc,
                       const char* warn) {
  ui_clear();
  char p[40];
  ui_truncate(p, prompt, 29);
  ui_text(2, 0, UI_TITLE, p);

  osk_field(buf);

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
  ui_truncate(f, warn ? warn : "A type B del  ST ok  SE cancel", 29);
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
  bool dirty = true;
  const char* warn = NULL;

  while (1) {
    if (cc >= rowlen(cr)) cc = rowlen(cr) - 1;   /* keep cursor in-row */
    if (dirty) { osk_render(prompt, buf, cr, cc, warn); dirty = false; }
    osk_vsync();

    u16 k = key_hit(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT |
                    KEY_A | KEY_B | KEY_START | KEY_SELECT);
    if (!k) continue;
    dirty = true;
    warn = NULL;

    if (k & KEY_SELECT) return false;
    else if (k & KEY_START) {
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
        return true;
      }
    }
    else if (k & KEY_A) { if (len < OSK_MAXLEN) { buf[len++] = KB[cr][cc]; buf[len] = 0; } }
    else if (k & KEY_B) { if (len > 0) buf[--len] = 0; }
    else if (k & KEY_UP)    { cr = (cr == 0) ? OSK_ROWS - 1 : cr - 1; }
    else if (k & KEY_DOWN)  { cr = (cr + 1) % OSK_ROWS; }
    else if (k & KEY_LEFT)  { int rl = rowlen(cr); cc = (cc == 0) ? rl - 1 : cc - 1; }
    else if (k & KEY_RIGHT) { int rl = rowlen(cr); cc = (cc + 1) % rl; }
  }
}
