# sd-browser — GBA SD file browser

A cartridge-native microSD file manager for the GBA (EZ-Flash Omega DE,
EverDrive GBA X5), built on the **gba-toolkit** shared layer. Part of the tool
family described in [`../../docs/ROADMAP.md`](../../docs/ROADMAP.md) (Project A).

## Status

**Code-complete (Phases 0–4), release candidate — builds clean.** The only
remaining gate is the hardware sign-off (the SD write path and the reboot are
not emulated). See [RELEASE-STATUS.md](RELEASE-STATUS.md) and the consolidated
[HARDWARE-SIGNOFF.md](HARDWARE-SIGNOFF.md).

**Phase 0 (read-only browser)** — runs on **both** carts; validated on real hardware:

- Directory listing with the `[..]` up-entry, folders sorted before files
- Navigation: UP/DOWN (auto-repeat), LEFT/RIGHT jump to top/bottom, L/R page up/down
- Sort by **Name / Size / Date** (SELECT cycles all 6 states) ascending/descending
- **Free / total space** on the status bar, plus a `sel/total` scroll-position indicator
- A **detail panel** under the list showing the highlighted entry's fuller name and its
  **date / time + size** (the complete name lives in Properties)
- File-type rows, and a read-only **file viewer** (Hex / Text, page-windowed so it
  handles files larger than RAM) via the actions menu's **View** item

**Phase 1 (write — EZ-Flash Omega only)** — validated on real hardware. Reached via the
actions menu (press **A** on a file, or **START** on a folder; write items appear
only on the Omega; the EverDrive stays read-only):

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
  **SELECT** marks all / clears all, the status bar shows the marked count + total
  size, and **START** opens a batch menu to **Copy / Cut / Delete** all marked
  items at once (and **Swap names** when exactly two are marked). **B** leaves
  selection mode. (Copy/Cut feed the same clipboard, so you then navigate and Paste.)

This completes the core file-manager feature set. Every write is confirmed and
logged to `sdbrowse_log.txt`.

**Phase 3 (in progress, Omega-only)** — an in-place **hex editor**. From the hex
viewer, **START** enters EDIT mode: a white-box cursor marks the editable byte,
**L/R** change its value, edited bytes show highlighted. **START** saves,
**SELECT** undoes pending edits, **B** exits. Saving is deliberately *not* an
in-place poke: it writes a temp copy with the edits applied, **byte-verifies** it
against original-with-edits, backs the original up to `<name>.bak~`, then
atomically renames the temp into place — so a failed write never corrupts the
file. File size is preserved (no insert/append). Pending **hardware sign-off**.

### On-screen keyboard — editing mid-text

The keyboard has a **movable caret**: **L / R** move it through the text (the
editable cell is shown as a white box with black text), so you can fix a mistake
in the middle without deleting everything. **A** inserts at the caret, **B**
backspaces before it.

**Phase 4 (settings, themes, reboot)** — quality-of-life, reached from the actions
menu (always present on **both** carts):

- **Settings menu** with persistent preferences saved to `/sdbrowse.cfg`
  (written on the Omega only; read on both carts — on the EverDrive settings are
  session-only and reset to defaults next launch). UP/DOWN pick a row, LEFT/RIGHT
  change the value (numeric rows auto-repeat on hold), **A** saves, **B** cancels
  and reverts. The settings: **theme**, **sort** (key + direction),
  **show hidden** files, **confirm before delete**, **default file viewer**
  (Hex/Text), **L/R jump distance**, **key-repeat delay**, **key-repeat speed**,
  **free-space unit** (B/KB/MB/GB), and **Reset to defaults** (L/R on that row;
  keeps the remembered folder).
- **5 themes** — Dark Blue (default), Dark Gray, Dark Green, Dark Purple, and a
  Light high-contrast scheme. The theme previews live as you cycle it.
- **Remembers the last folder** — reopens it on next launch (falls back to root
  if it no longer exists).
- **Reboot to loader** *(experimental)* — soft-reboot toward the cart's loader
  menu (EZ-Flash kernel / EverDrive OS) instead of power-cycling. Confirmed
  working on the EZ-Flash Omega DE; the EverDrive path is still being validated.
  Writes no data either way.

**More file operations** — added to the actions / batch / viewer:

- **Find…** — recursive keyword search from the current folder: type a keyword and
  get a scrollable list of every file/folder whose name contains it (ASCII
  case-insensitive). Picking a result drops you in that item's folder with it
  highlighted. Read-only, works on **both** carts. (Search everything by going to
  `/` first.)
- **Folder size** — recursively totals a folder's bytes + file/subfolder counts
  (read-only, works on **both** carts).
- **Duplicate** — copies the selected file/folder in place under an auto-suffixed
  name (`foo.txt` → `foo (copy).txt`); folders duplicate recursively. *(Omega)*
- **New file** — creates an empty file via the keyboard (then fill it with the
  hex editor). *(Omega)*
- **Swap names** — in selection mode, mark **exactly two** items and pick "Swap
  names" to exchange their filenames (a safe 3-way rename). *(Omega)*
- **Go to offset** — in the file viewer, jump straight to a hex byte offset
  instead of paging there.

The actions menu now **scrolls** (▲/▼ indicators) when it holds more items than
fit on screen, so nothing spills past the box.

**File-type viewers (Phase 6)** — the actions menu routes a file to a viewer by
its extension, with **View (hex/text)** always available as the fallback:

- **Open by type** — a small extension→viewer registry. A recognized type adds an
  "Open" row above "View"; everything else just uses the hex/text viewer.
- **Image viewer (BMP)** — "View image" on a `.bmp` decodes it to the screen,
  integer-downscaled to fit 240×160 (uncompressed 8/16/24/32-bit). Read-only,
  both carts. **B** returns.
- **Word-wrapped text** — the text viewer now wraps at word boundaries, expands
  tabs, skips CR, and handles line breaks (the font is ASCII; high bytes show as
  `.`).

> **Honest scope:** real **MP3/AAC playback, MP4/H.264 video, and PDF rendering
> are not feasible** on the GBA's 16.78 MHz CPU (+ the OS-mode SD constraint).
> The roadmap adds *metadata/info* views (ID3 tags, MP4 codec/resolution, PDF page
> count) and more image formats (PNG/GIF/JPEG) — see
> [`../../docs/ROADMAP.md`](../../docs/ROADMAP.md) §A Phase 6. We never pretend to
> play/render what the hardware can't.

## Controls

| Key | Action |
|-----|--------|
| UP / DOWN | Move cursor (hold to repeat) |
| LEFT / RIGHT | Jump up / down by the configured distance (default 11 rows) |
| L / R | Page up / page down (one screen) |
| A | **Folder:** open it · **File:** open the **actions menu** (a type-specific **Open** e.g. *View image* for `.bmp` / **View** hex-text / Info / **Find…**; **Settings…** and **Reboot to loader…** always; and on Omega: rename / copy / cut / duplicate / paste here / attributes / delete / new file / new folder / select multiple) |
| START | **Any item:** open the **actions menu** (the same menu A opens on a file — **View** lives inside it; this is also how you reach Settings/Reboot/Find when nothing is selected) |
| B | Up one folder |
| SELECT | Cycle the sort: 6 states across Name / Size / Date, each ascending and descending. The status bar spells out the active one — "Name A-Z", "Name Z-A", "Size small-big", "Size big-small", "Date old-new", "Date new-old". Folders always sort before files. |

> **Note:** START opens the actions menu on any item; on a file that's the same
> menu as A (which holds **View**). SELECT cycles the sort. START stays the
> consistent primary/confirm button in the keyboard, hex-editor save, and settings.

### Selection mode (from "Select multiple")

| Key | Action |
|-----|--------|
| A | Mark / unmark the highlighted entry (`*`) |
| SELECT | Mark all / clear all |
| START | Batch menu: Copy / Cut / Delete (and **Swap names** when exactly 2 are marked) |
| B | Leave selection mode |

### Settings menu

| Key | Action |
|-----|--------|
| UP / DOWN | Pick a setting |
| LEFT / RIGHT | Change the highlighted value (numeric rows auto-repeat on hold; theme/sort/toggles step once per press) |
| A / START | Save and close |
| B | Cancel — revert every change, including the live theme preview |

### Find results

| Key | Action |
|-----|--------|
| UP / DOWN | Move through the matches (hold to repeat) |
| A / START | Go to the highlighted match (opens its folder with it selected) |
| B | Back to the browser |

### On-screen keyboard (QWERTY)

| Key | Action |
|-----|--------|
| d-pad | Move the character cursor (hold to repeat); rows: numbers, qwerty, lower, upper, symbols + space |
| L / R | Move the text caret left / right (hold to repeat; edit mid-text, caret shown as a white box) |
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
| SELECT | Go to offset (type a hex offset) |
| START | (hex, Omega) enter EDIT mode |
| B | Back to the browser |

### Hex editor (EDIT mode)

| Key | Action |
|-----|--------|
| d-pad | Move the byte cursor (white box); auto-scrolls |
| L / R | Decrease / increase the cursor byte (hold to repeat) |
| START | Save (confirm; original kept as `<name>.bak~`) |
| SELECT | Undo all pending edits |
| B | Leave EDIT mode (prompts if there are unsaved edits) |

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
    main.c        # bring-up + browser UI/state machine; actions/settings/reboot
    fs_ops.c/.h   # pure-C file ops: list / sort / free-space / copy / delete (host-testable)
    osk.c/.h      # on-screen QWERTY keyboard with a movable caret
    ui.c/.h       # Mode-3 bitmap UI layer (vendored from the record-mixer)
    theme.c/.h    # 5 runtime color themes; UI_* macros read the active g_theme
    cfg.c/.h      # /sdbrowse.cfg INI settings (load both carts, save Omega-only)
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
