# sd-browser — GBA SD file browser

A cartridge-native microSD file manager for the GBA (EZ-Flash Omega DE,
EverDrive GBA X5), built on the **gba-toolkit** shared layer. Part of the tool
family described in [`../../docs/ROADMAP.md`](../../docs/ROADMAP.md) (Project A).

## Status

**Phase 0 (read-only browser)** — runs on **both** carts; validated on real hardware:

- Directory listing with the `[..]` up-entry, folders sorted before files
- Navigation: UP/DOWN (auto-repeat), LEFT/RIGHT jump to top/bottom, L/R page up/down
- Sort by **Name / Size / Date** (START cycles all 6 states) ascending/descending
- **Free / total space** on the status bar, plus a `sel/total` scroll-position indicator
- A **detail panel** under the list showing the highlighted entry's fuller name and its
  **date / time + size** (the complete name lives in Properties)
- File-type rows, and a read-only **file viewer** (Hex / Text, page-windowed so it
  handles files larger than RAM) via `A` on a file

**Phase 1 (write — EZ-Flash Omega only)** — validated on real hardware. Reached via the
**SELECT** actions menu (write items appear only on the Omega; the EverDrive stays
read-only):

- An **on-screen keyboard** (QWERTY d-pad grid, both cases shown) for typed names,
  with FAT-name validation
- **New folder** (`f_mkdir`), **Delete** file or folder (recursive, with a confirm
  prompt; clears read-only as needed), and **attribute toggles** (read-only / hidden)
- **Properties** screen showing the **full name (wrapped)**, type, size, modified
  date/time, and attributes

**Phase 2 (in progress, Omega-only)** — via the actions menu:
- **Rename / move** (`f_rename`), keyboard pre-filled with the current name
- **Copy / Cut / Paste** with a clipboard: Copy or Cut an entry, navigate to a
  destination folder, then **Paste here**. Files and **whole folders** copy
  recursively; Cut is a same-volume move. Pasting onto an existing item asks to
  overwrite; pasting a folder into its own subtree is refused. A footer
  indicator shows what's on the clipboard (`[COPY]`/`[CUT]`).

- **Multi-select + batch** — from the actions menu, "Select multiple" enters a
  selection mode: **A** marks/unmarks the highlighted entry (shown with a `*`),
  **START** marks all / clears all, the status bar shows the marked count + total
  size, and **SELECT** opens a batch menu to **Copy / Cut / Delete** all marked
  items at once. **B** leaves selection mode. (Copy/Cut feed the same clipboard,
  so you then navigate and Paste.)

This completes the core file-manager feature set. Every write is confirmed and
logged to `sdbrowse_log.txt`.

### On-screen keyboard — editing mid-text

The keyboard has a **movable caret**: **L / R** move it through the text (the
editable cell is shown as a white box with black text), so you can fix a mistake
in the middle without deleting everything. **A** inserts at the caret, **B**
backspaces before it.

## Controls

| Key | Action |
|-----|--------|
| UP / DOWN | Move cursor (hold to repeat) |
| LEFT / RIGHT | Jump to top / bottom of the listing |
| L / R | Page up / page down |
| A | Open folder · (on a file) open the Hex/Text viewer |
| B | Up one folder |
| START | Cycle sort: Name → Size → Date, each `^` ascending / `v` descending (6 states) |
| SELECT | Open the **actions menu** (Info, and on Omega: rename / copy / cut / paste here / attributes / delete / new folder / select multiple) |

### Selection mode (from "Select multiple")

| Key | Action |
|-----|--------|
| A | Mark / unmark the highlighted entry (`*`) |
| START | Mark all / clear all |
| SELECT | Batch menu: Copy / Cut / Delete the marked items |
| B | Leave selection mode |

### On-screen keyboard (QWERTY)

| Key | Action |
|-----|--------|
| d-pad | Move the character cursor (rows: numbers, qwerty, lower, upper, symbols + space) |
| L / R | Move the text caret left / right (edit mid-text; caret shown as a white box) |
| A | Insert the highlighted character at the caret |
| B | Backspace (delete before the caret) |
| START | Confirm |
| SELECT | Cancel |

### In the file viewer

| Key | Action |
|-----|--------|
| A | Toggle Hex ↔ Text |
| UP / DOWN | Scroll one row |
| L / R | Page up / page down |
| LEFT / RIGHT | Jump to start / end of file |
| B | Back to the browser |

## Build

Requires only Docker (no local toolchain):

```sh
./build.sh
```

Or, with a local devkitPro `gba-dev` environment, from this directory:

```sh
make rebuild
```

Output: `sd_browser.gba` (ROM title `SDBROWSE`). Copy it to the flashcart's SD
card and launch it like any other GBA ROM.

### How the build finds the shared layer

This tool keeps only its own UI/logic in `source/`. The Makefile pulls the
shared hardware + filesystem layer from the repo root via relative paths
(`../../lib`, `../../lib/fatfs`, `../../lib/ezflashomega`,
`../../lib/everdrivegbax5`, and `../../source` for `gba_rtc` + `log`). `build.sh`
therefore mounts the **repo root** into the container and runs `make` here.

## Layout

```
projects/sd-browser/
  Makefile        # adapted from the repo root; SRCDIRS/INCDIRS add ../../{lib,source}
  build.sh        # Docker build (mounts repo root)
  source/
    main.c        # bring-up + browser UI/state machine (Phase 0)
    fs_ops.c/.h   # pure-C file ops: list / sort / free-space (host-testable)
    ui.c/.h       # Mode-3 bitmap UI layer (vendored from the record-mixer)
```

## Conventions inherited from the toolkit

- **Write is Omega-only** (EverDrive write is not wired); Phase 0 is read-only so
  it runs on both carts. Write features will gate on `active_flashcart == EZ_FLASH_OMEGA`.
- **OS-mode rule:** the ROM is unmapped during any SD transfer — never render
  mid-transfer. (Phase 0 only reads via FatFs, which handles this internally.)
- **No big buffers on the IWRAM stack:** the directory listing lives in
  `EWRAM_BSS`; `fs_ops` takes caller-owned storage.
- **Pure-C core:** `fs_ops.c` avoids tonc/GBA headers so it can be host-tested
  against a FatFs image.

Anything touching the SD write path, the RTC, or user data is **not done until
`hardware-testing-protocol` signs off** — the SD path is not emulated.
