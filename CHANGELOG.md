# Changelog

All notable changes to **File-Browser-GBA**. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions are the Git tags /
GitHub releases.

## [Unreleased]
### Added (v1.1.0 candidate - UI/SD behaviour is hardware-unverified)
- **On-screen text editor** (actions menu -> *Edit text*, Omega-only, files up to 32 KiB).
  Navigate mode (D-pad caret, L/R page, A keyboard, SELECT undo, START menu, B exit)
  and Type mode (3-page on-screen keyboard + Space/Enter/Tab). Keeps the file's own
  CRLF/LF line endings, shows `Ln/Col/*/undo/CRLF`, warns before opening binary
  bytes (kept as-is on save). Saves through a verified write (`<file>.txtnew~` ->
  byte-compare -> original kept as `<file>.bak~`); refuses with a clear message if a
  leftover `.txtnew~` exists or the `.bak~` shares data with the file.
- **Pin to top** - pin any file/folder; pinned paths are the first rows of every
  folder in List, Grid and Column views (A jumps there, START = Open/Unpin; a
  missing target offers to unpin). Stored in `/file_browser_gba/pins.txt`.
- **START-menu shortcuts** - *Add shortcut* / *Remove shortcut*; saved shortcuts are
  the first rows of the START menu in every folder (A jumps, SELECT removes; up to
  16). Stored in `/file_browser_gba/shortcuts.txt`.
- **Button combos** - SELECT + UP/DOWN/LEFT/RIGHT/A/B/L/R each run a bound folder,
  file or action (Settings, Find, Trash, Paste here, New folder/file, Select
  multiple, show/hide hidden, cycle view, go to root, reboot to loader). Bind via
  *Bind to button...* in the actions menu or Settings -> *Button shortcuts...*;
  stored in `/file_browser_gba/buttons.txt`.
### Changed
- **One folder on the card**: the settings and log now live in `/file_browser_gba/`
  (`settings.cfg`, `log.txt`). The old root `/file_browser_gba.cfg` is still read if
  the new file is absent (and left alone).
- **SELECT cycles the sort order on release** (not on press), and only when no other
  key was pressed while it was held, so it can act as the combo modifier. Inside
  multi-select mode SELECT is unchanged.
- The Find results and the editor buffer now share one EWRAM overlay (no extra RAM
  for the 32 KiB buffer).
### Fixed
- **Intermittent hang at "Detecting flashcart..."** on the EZ-Flash Omega DE: the
  cart-detection code used to identify the currently-running page by a single ROM
  header word, which is identical for every build sharing a title. If an older
  same-titled copy of this tool was still sitting in PSRAM (the last SD load) or on
  a lower NOR page, booting a newer copy from NOR could map the stale copy instead
  and hang before the file list ever showed. Detection now verifies the page's
  actual content, not just its title, before trusting it.
- **A "no cartridge detected" failure now reaches the halt screen.** In the rare
  case where detection could not find the running image on any page, the tool used
  to freeze with a blank screen; it now shows `No flashcart! det=6 pg=ffff` so the
  state is reportable. (Reliable when the tool was launched from the SD card.)

## [1.0.0] — 2026-06-29
### Changed
- **1.0 — stable.** No functional changes since 0.12.0 — the same hardware-validated
  ROM, now declared a stable 1.0 release.
- The reworked **Trash view** (SELECT sort cycle, name↔origin-path rows, restore-in-
  view, the days-left countdown) is now **hardware-validated** on a GBA SP + EZ-Flash
  Omega DE — the last item that was gating 1.0 (closes #3). The whole tool (P0–P7 —
  every read and write path, the recycle bin, and auto-clear) has now been confirmed
  on the real cartridge.

## [0.12.0] — 2026-06-18
### Added
- Shared **clipboard `[CUT]`/`[COPY]` footer** now shows in **all three views**
  (List, Grid, Columns) — previously only the List view hinted that something was
  on the clipboard.
- This `CHANGELOG.md`.
### Docs
- README: documented that the **EverDrive GBA X5 is read-only by design** (its
  write path isn't wired) and added a **Screenshots** slot.

## [0.11.0] — 2026-06-18
### Added
- **Column (Miller) view** — a macOS-Finder-style two-pane cascade: the left pane
  is the focused folder, the right pane previews the highlighted entry (a folder's
  contents, or a file's info). **RIGHT/A** descends, **LEFT/B** ascends, with a
  path breadcrumb header. Completes the three view modes (List · Grid · Columns).

## [0.10.0] — 2026-06-18
### Added
- **Grid / icon view** — theme-colored folder/file glyphs at a chosen density
  (**3–6 columns**). 2D d-pad navigation (LEFT/RIGHT wrap, UP/DOWN jump a row,
  L/R page); the selected tile's full name + date/size show on the status line.
- A **View** setting (List / Grid x3–x6 / Columns), persisted to the config.

## [0.9.0] — 2026-06-18
### Added
- **5 vivid themes** — Pink, Red, Orange, Orange-Black, Blue-Black (10 total).
- **Trash view v2** — **SELECT** cycles the sort (newest/oldest deleted, name
  A-Z/Z-A, origin-path A-Z/Z-A); **START** options toggle rows between name and
  **origin path** and offer **Empty Trash**; every row shows a **days-left**
  countdown before auto-clear when that's enabled.

## [0.8.0] — 2026-06-15
### Added
- **Auto-clear old trash** — optional Settings value (Off by default, 1–365 days)
  that deletes trashed items older than N days at launch; fails safe with no RTC.
### Changed
- The recycle bin (Trash) is **hardware-validated** (GBA SP + EZ-Flash Omega DE).
- Reboot-to-loader de-labelled "experimental" (confirmed on the Omega DE).
- File actions menu: **Info / properties** moved above **View (hex/text)**.

## [0.7.0] — 2026-06-15
### Added
- First public release. A cartridge-native microSD **file manager** for the GBA:
  browse / sort / search; new file+folder, rename/move, copy/cut/paste (recursive),
  duplicate, delete; the **recycle bin (Trash)** with restore + empty; an in-place
  **verified hex editor**; Hex / word-wrapped-Text / **BMP** viewers; 5 themes;
  persistent settings; reboot-to-loader. EverDrive GBA X5 runs read-only.

[1.0.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v1.0.0
[0.12.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.12.0
[0.11.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.11.0
[0.10.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.10.0
[0.9.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.9.0
[0.8.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.8.0
[0.7.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.7.0
