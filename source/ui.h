#ifndef UI_H
#define UI_H

#include <tonc.h>
#include <stdbool.h>
#include "theme.h"

/* Bitmap (Mode 3) UI helper layer: filled panels, bordered boxes, colored text,
 * and a solid selection-highlight bar. Text still goes through libtonc TTE
 * (tte_write/tte_set_ink/tte_set_pos), but is rendered into the Mode 3 bitmap.
 * Initialise the mode + TTE in main (see ui_init).
 *
 * Vendored from the Pokemon record-mixer reference impl (source/ui.c/.h). */

#define UI_SCR_W   240
#define UI_SCR_H   160
#define UI_ROW_H   8            /* sys8 font line height (px)                   */
#define UI_COLS    30           /* 240/8 fixed-width columns                    */

/* Palette — now reads the active runtime theme (theme.h) so the UI re-colors
 * live when the user switches themes; no call-site changes needed. */
#define UI_BG       (g_theme.bg)        /* screen background          */
#define UI_PANEL    (g_theme.panel)     /* panel fill                 */
#define UI_BORDER   (g_theme.border)    /* panel/divider border       */
#define UI_TEXT     (g_theme.text)      /* primary text               */
#define UI_DIM      (g_theme.dim)       /* secondary/dim text         */
#define UI_TITLE    (g_theme.title)     /* headers                    */
#define UI_SEL      (g_theme.sel)       /* selection highlight bar    */
#define UI_SELTEXT  (g_theme.seltext)   /* text on the highlight bar  */
#define UI_OK       (g_theme.ok)        /* good/green                 */
#define UI_WARN     (g_theme.warn)      /* warning/orange             */
#define UI_DIRCLR   (g_theme.dirclr)    /* directory rows             */
#define UI_SAVECLR  (g_theme.saveclr)   /* file rows                  */

/* Switch to Mode 3 and init bitmap TTE with the fixed 8x8 system font. */
void ui_init(void);

/* Clear the whole screen to UI_BG. */
void ui_clear(void);

/* Filled rectangle with a 1px border. (x,y) top-left, w/h in pixels. */
void ui_panel(int x, int y, int w, int h, u16 fill, u16 border);

/* A horizontal divider line at pixel row y, from x..x+w. */
void ui_hline(int x, int y, int w, u16 color);

/* Draw text at pixel (x,y) in colour `ink`. */
void ui_text(int x, int y, u16 ink, const char* s);

/* Draw a list row of width `w` px at (x,y). When `selected`, paints a UI_SEL
 * bar behind it and uses UI_SELTEXT; otherwise draws the text in `ink`. */
void ui_text_sel(int x, int y, int w, bool selected, u16 ink, const char* s);

/* Copy `in` into `out` clamped to `max_cols` display columns, UTF-8-safe
 * (never splits a codepoint); appends '~' as the last column if truncated.
 * `out` must hold at least max_cols*4 + 1 bytes to be safe. */
void ui_truncate(char* out, const char* in, int max_cols);

/* Wrappers around tonc's key_repeat_mask that also remember the current mask,
 * so a modal can save the caller's mask on entry and restore it exactly on exit
 * (instead of hard-coding a value and clobbering a nested caller's mask). */
void ui_set_repeat_mask(u16 mask);
u16  ui_get_repeat_mask(void);

#endif /* UI_H */
