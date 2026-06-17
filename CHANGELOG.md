# Changelog

All notable changes to **File-Browser-GBA**. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions are the Git tags /
GitHub releases.

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

[0.12.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.12.0
[0.11.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.11.0
[0.10.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.10.0
[0.9.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.9.0
[0.8.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.8.0
[0.7.0]: https://github.com/GuyShtainer/File-Browser-GBA/releases/tag/v0.7.0
