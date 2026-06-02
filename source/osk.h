#ifndef OSK_H
#define OSK_H

#include <stdbool.h>

/*
 * Modal on-screen keyboard for the SD file browser (Phase 1). The GBA has no
 * physical keyboard, so any typed name (mkdir / rename / new file) goes through
 * this d-pad grid. It runs its own frame loop and blocks until the user
 * confirms (START) or cancels (SELECT).
 *
 * Keys: d-pad moves the grid cursor, A types the highlighted character,
 * B = backspace, START = confirm, SELECT = cancel.
 *
 * On confirm the entered text is validated as a FAT-safe name (non-empty; no
 * control chars; none of \ / : * ? " < > | ; no leading/trailing space; no
 * trailing dot; not "." or ".."), copied into out[0..cap-1], and true is
 * returned. On cancel (or if cap is too small) it returns false. `initial`
 * pre-fills the buffer (may be NULL).
 */
bool osk_input(const char* prompt, const char* initial, char* out, int cap);

#endif /* OSK_H */
