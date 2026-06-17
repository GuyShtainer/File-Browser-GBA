#ifndef THEME_H
#define THEME_H

#include <tonc.h>

/*
 * Runtime color theme. The UI_* macros in ui.h read the active theme (g_theme)
 * instead of compile-time RGB15 literals, so switching themes is just a struct
 * copy — every existing ui_text(..., UI_TITLE, ...) call picks it up on the next
 * render with no call-site changes. Field order MUST match the UI_* macros and
 * the THEMES[] initializers in theme.c.
 */
typedef struct {
  u16 bg, panel, border, text, dim, title, sel, seltext, ok, warn, dirclr, saveclr;
} Theme;

extern Theme g_theme;            /* the active theme; UI_* macros dereference this */

#define THEME_COUNT 10           /* Dark Blue, Dark Gray, Dark Green, Dark Purple, Light,
                                  * Pink, Red, Orange, Orange-Black, Blue-Black */

void        theme_apply(int idx);   /* copy THEMES[idx] into g_theme (idx clamped) */
const char* theme_name(int idx);    /* short label for the settings menu */

#endif /* THEME_H */
