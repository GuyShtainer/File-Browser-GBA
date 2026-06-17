#include "theme.h"

/* tonc's RGB15() is an INLINE function, so it cannot appear in a static
 * initializer (must be a constant expression). C() is the same packing done as
 * a constant macro: COLOR = r | g<<5 | b<<10, each channel 0..31 (RGB15). */
#define C(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

/* Field order: bg, panel, border, text, dim, title, sel, seltext, ok, warn, dirclr, saveclr.
 * The first four are dark schemes (high-contrast light text on a dark panel); the
 * last is a light high-contrast scheme (dark text on a light panel). */
typedef struct { const char* name; Theme t; } NamedTheme;

static const NamedTheme THEMES[THEME_COUNT] = {
  { "Dark Blue", {                                   /* current / default */
      C( 1, 2, 4), C( 2, 4, 9), C(10,13,20), C(31,31,31),
      C(17,18,21), C( 8,28,31), C( 5,10,24), C(31,31,18),
      C( 8,28,10), C(31,18, 3), C(10,24,31), C(31,31,31) } },
  { "Dark Gray", {
      C( 2, 2, 2), C( 4, 4, 6), C(12,12,14), C(30,30,30),
      C(16,16,18), C( 0,28,31), C( 8, 8,16), C(31,31,20),
      C( 4,24, 6), C(28,14, 0), C( 6,20,28), C(28,28,28) } },
  { "Dark Green", {
      C( 1, 4, 2), C( 2, 6, 3), C( 8,14, 9), C(28,31,28),
      C(14,18,15), C( 4,26,20), C( 4,12, 6), C(28,31,16),
      C( 8,28, 8), C(30,18, 2), C( 6,22,24), C(26,28,26) } },
  { "Dark Purple", {
      C( 3, 1, 5), C( 5, 2, 8), C(12, 6,18), C(31,30,31),
      C(18,14,22), C(16,16,31), C( 6, 3,14), C(31,24,31),
      C( 6,28, 8), C(31,16, 4), C(12,12,31), C(30,28,30) } },
  { "Light", {                                       /* dark text on a light panel */
      C(28,28,29), C(25,26,28), C(14,15,17), C( 2, 2, 4),
      C(11,12,14), C( 2, 4,20), C(16,21,31), C( 0, 0, 8),
      C( 2,14, 3), C(22, 7, 0), C( 3, 6,22), C( 3, 3, 5) } },
  /* --- vivid schemes --- */
  { "Pink", {                                         /* hot pink on deep plum */
      C( 4, 0, 4), C( 8, 1, 7), C(22, 4,17), C(31,28,31),
      C(23,14,21), C(31, 6,22), C(20, 2,12), C(31,31,31),
      C(12,30,16), C(31,21, 5), C(31,15,27), C(31,25,30) } },
  { "Red", {                                          /* vivid red on near-black */
      C( 4, 0, 0), C( 8, 1, 1), C(22, 4, 4), C(31,28,28),
      C(23,12,12), C(31, 5, 5), C(18, 2, 2), C(31,31,29),
      C(13,30,12), C(31,24, 6), C(31,13,10), C(31,24,22) } },
  { "Orange", {                                       /* vivid orange on dark brown */
      C( 4, 1, 0), C( 8, 3, 0), C(25,12, 2), C(31,29,25),
      C(25,17, 8), C(31,18, 2), C(17, 7, 0), C(31,30,22),
      C(14,30,10), C(31,27, 4), C(31,20, 3), C(31,27,17) } },
  { "Orange-Black", {                                 /* bright orange on pure black */
      C( 0, 0, 0), C( 3, 1, 0), C(22,11, 0), C(31,23, 7),
      C(19,12, 2), C(31,16, 0), C(13, 6, 0), C(31,29,14),
      C(16,30, 8), C(31,25, 0), C(31,20, 2), C(29,19, 5) } },
  { "Blue-Black", {                                   /* electric blue on pure black */
      C( 0, 0, 1), C( 0, 2, 4), C( 2,11,24), C(15,23,31),
      C( 6,13,23), C( 4,19,31), C( 0, 5,15), C(21,29,31),
      C( 8,30,18), C(31,18, 4), C( 6,21,31), C(17,25,31) } },
};

/* Default = Dark Blue (matches the original hardcoded palette); main() calls
 * theme_apply() from the loaded config at startup. */
Theme g_theme = {
  C( 1, 2, 4), C( 2, 4, 9), C(10,13,20), C(31,31,31),
  C(17,18,21), C( 8,28,31), C( 5,10,24), C(31,31,18),
  C( 8,28,10), C(31,18, 3), C(10,24,31), C(31,31,31),
};

void theme_apply(int idx) {
  if (idx < 0 || idx >= THEME_COUNT) idx = 0;
  g_theme = THEMES[idx].t;
}

const char* theme_name(int idx) {
  if (idx < 0 || idx >= THEME_COUNT) idx = 0;
  return THEMES[idx].name;
}
