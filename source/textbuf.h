#ifndef TEXTBUF_H
#define TEXTBUF_H

/*
 * Text-editing core for the on-screen text editor (pure C).
 *
 * Holds ONE file in memory as a flat byte array and provides insert/delete with
 * undo, plus the display layout (word wrap + tab stops + caret cells) the UI
 * draws. Includes only <stdint.h>/<stdbool.h>/<string.h> — NO tonc/GBA headers —
 * so tests/host_text_test.c dual-compiles and runs it on the PC (toolkit
 * convention 5). The UI layer (txtedit.c) owns all rendering and input.
 *
 * Byte-exact by construction: bytes the user never touches are written back
 * verbatim, so editing a file that contains binary/UTF-8 bytes does not corrupt
 * the parts left alone (they are DISPLAYED as '.', but stored unchanged).
 *
 * Layout model — one cell per byte, except:
 *   '\n'          ends the row and contributes the row's end-of-line caret cell
 *   '\r' of CRLF  zero width (the pair shows as one line break)
 *   '\t'          expands to the next 4-column stop (all cells map to the tab)
 *   >0x7E / <0x20 shown as '.'
 * Every row carries the byte offset of each cell, so the caret (a byte offset)
 * always maps to an exact screen cell and back.
 */

#include <stdint.h>
#include <stdbool.h>

/* Max editable file size. The buffer is CALLER-OWNED (tb_attach): the browser
 * overlays it on the Find-results array (dead while an editor is open). Bigger
 * files stay viewable/hex-editable — the editor refuses them. */
#define TB_CAP      32768

#define TB_COLS     29          /* text columns; cell TB_COLS holds an end cell */
#define TB_MAXROWS  17          /* rows when the keyboard is hidden             */

typedef struct {
  uint32_t start;               /* byte offset this row begins at               */
  uint32_t next;                /* byte offset of the next row (== start = last) */
  uint32_t off[TB_COLS + 1];    /* byte offset shown in each cell               */
  uint8_t  ch[TB_COLS + 1];     /* printable glyph for each cell                */
  uint8_t  ncell;               /* cells in use (<= TB_COLS + 1)                */
  uint8_t  endcell;             /* 1 = the LAST cell is a caret-only cell (the   */
                                /*     end of a line break, or past-the-end)    */
} TbRow;

/* ---- buffer ------------------------------------------------------------- */

/* Give the core its memory (`cap` bytes, clamped to TB_CAP). Must be called
 * once before tb_load. Resets to an empty buffer. */
void     tb_attach(uint8_t* mem, uint32_t cap);

/* Load `n` bytes (n <= TB_CAP) as the edit buffer; clears undo + the dirty flag
 * and auto-detects the file's dominant line ending. Returns false (buffer left
 * empty) if n > TB_CAP. `data` may be NULL only when n == 0. */
bool     tb_load(const uint8_t* data, uint32_t n);   /* false also if not attached */

uint32_t       tb_len(void);
const uint8_t* tb_data(void);              /* contiguous; valid for tb_len() bytes */
uint8_t        tb_at(uint32_t i);          /* 0 past the end                       */
bool           tb_dirty(void);             /* edited since load / last tb_clean()  */
void           tb_clean(void);             /* mark saved                           */
bool           tb_crlf(void);              /* file uses CRLF line endings          */

/* Insert / delete, recording undo. Both return false and change nothing when the
 * edit would not fit (insert) or is out of range (delete). */
bool tb_insert(uint32_t pos, const uint8_t* s, uint32_t n);
bool tb_delete(uint32_t pos, uint32_t n);

/* Insert one line break in the file's own convention (CRLF or LF). */
bool tb_newline(uint32_t pos);

/* Undo the most recent edit. Returns false when the undo log is empty; on
 * success *out_caret (if non-NULL) receives where the caret should land. */
bool tb_undo(uint32_t* out_caret);
int  tb_undo_count(void);

/* ---- layout / caret ----------------------------------------------------- */

/* Lay out up to `nrows` display rows starting at byte offset `top` (which must
 * be a row start). Returns the number of rows written. A row's `next` equals its
 * `start` exactly when it is the LAST row of the buffer. Rows hard-wrap at
 * TB_COLS, preferring to break after a space/tab when the word does not fit. The last row of the
 * buffer carries a trailing cell for the past-the-end caret. */
int tb_layout(uint32_t top, int nrows, TbRow* rows);

/* Row-start arithmetic (both return a valid row start). */
uint32_t tb_row_start_of(uint32_t off);    /* start of the row containing `off`  */
uint32_t tb_prev_row_start(uint32_t off);  /* start of the row before row `off`  */

/* Caret movement. All results are "snapped": the caret never lands BETWEEN the
 * CR and LF of a CRLF pair (a break is crossed in one step; the caret sits at
 * the CR's offset = the row's end cell). */
uint32_t tb_caret_left(uint32_t caret);
uint32_t tb_caret_right(uint32_t caret);
uint32_t tb_caret_up(uint32_t caret);
uint32_t tb_caret_down(uint32_t caret);
uint32_t tb_caret_home(uint32_t caret);    /* start of the current display row   */
uint32_t tb_caret_end(uint32_t caret);     /* end of the current display row     */
uint32_t tb_snap(uint32_t caret);          /* clamp to [0,len] and off a CRLF CR */

/* How many bytes a backspace at `caret` removes (0, 1, or 2 for a CRLF pair). */
uint32_t tb_backspace_len(uint32_t caret);

/* 1-based line number and 1-based byte column of `caret` (for the status line). */
uint32_t tb_line_of(uint32_t caret);
uint32_t tb_col_of(uint32_t caret);

/* True if the buffer holds a NUL or any byte >= 0x80 (shown as '.'; unchanged on
 * save, but the user should know the view is lossy). */
bool tb_has_binary(void);

#endif /* TEXTBUF_H */
