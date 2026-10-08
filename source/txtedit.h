#ifndef TXTEDIT_H
#define TXTEDIT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * On-screen text editor (Mode 3 UI). Loads `path` into the caller-owned buffer
 * `mem` (`cap` bytes; the browser overlays it on the dead Find-results array),
 * runs its own frame loop until the user exits, and saves through the verified
 * fsop_save_buffer ("<path>.txtnew~" -> byte-compare -> ".bak~" + rename).
 *
 * Navigate mode: D-pad caret, L/R page, A = keyboard, SELECT = undo,
 * START = menu (Save / Undo / Exit), B = exit.
 * Type mode (keyboard panel): D-pad = key cursor, A = type, B = backspace,
 * L/R = caret left/right, SELECT = next key page, START = close keyboard.
 *
 * Returns true if the file was saved at least once (the browser must rescan).
 * Write-gating (Omega only), the size limit and the read-only attribute are
 * checked by the caller. Asks before opening a file that holds binary bytes.
 */
bool txtedit_run(const char* path, const char* name, uint8_t* mem, uint32_t cap);

#endif /* TXTEDIT_H */
