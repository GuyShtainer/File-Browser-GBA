# File-Browser-GBA — release status

**Status: FEATURE-COMPLETE (Phases 0–7), release candidate. Builds clean (zero
warnings). The one remaining gate is the hardware sign-off.**

`file_browser_gba.gba` — ROM ~130 KB, IWRAM ~11.9 KB / 32 KB, EWRAM well within 256 KB.

**Self-contained:** the shared hardware/FS layer (flashcartio, FatFs, the
per-cart block drivers, the cartridge RTC and the logger) is vendored into
`lib/` + `source/`, so the repo builds standalone (`./build.sh`, or `make
rebuild` with a local devkitARM) with no external checkout.

## Certified now (software level)
- **Features complete** — P0 read-only browser (nav/sort/properties/free-space/
  hex+text viewer); P1 on-screen keyboard + mkdir/delete/attributes; P2
  rename/move + copy/cut/paste (incl. recursive folder copy) + multi-select
  batch + editable-caret keyboard; P3 in-place hex editor; P4 settings/themes/
  reboot + find/folder-size/duplicate/new-file/swap-names/go-to-offset; P6
  open-by-type + BMP image viewer + word-wrapped text; **P7 recycle bin (Trash):
  delete-to-trash, restore-to-origin (collision-safe + root fallback), delete-
  forever, empty-trash, batch-trash — all atomic same-volume moves.**
- **Trash data-safety reviewed** — an adversarial multi-agent pass confirmed the
  recycle bin never corrupts data (atomic `f_rename` moves, never-overwrite
  restore) and closed two footguns: `/.sdtrash` is hidden from normal browsing
  and `last_dir` can't point inside it; trashing a reserved-namespace name
  (`*.origin~`/`*.sdbtmp~`) is refused.
- **Reviewed** — every feature was specialist-reviewed as built, plus a final
  release-readiness audit (write-safety regression sweep, Omega-gating + OS-mode
  audit, whole-UI integration sweep). All confirmed:
  - **Data safety:** every mutating op (mkdir/chmod/rename/delete/copy/paste/
    hex-edit) upholds "never leave the user with neither old nor new" / clean
    failure. Overwrite + hex-save use a verified swap (write temp → byte-compare
    → `.bak~` backup → atomic rename). Reserved temp namespaces
    (`.sdbtmp~`/`.bak~`/`.hexnew~`) can't clobber user data. Big buffers in EWRAM.
  - **Write gating:** every write (incl. select-multiple, new-folder, hex-edit
    launch, batch) is behind `can_write() == EZ_FLASH_OMEGA`; EverDrive is
    read-only by construction.
  - **OS-mode:** no render mid-transfer; live `FIL` closed before the hex-save
    renames; VBlank handler NULL; no-retry write results surfaced on every path.
- **Fixed in this pass:** 3 UTF-8 filename stack-buffer overflows (an SD-card
  name of 4-byte codepoints could exceed an on-screen-string buffer); all
  truncation chains now sized for the `~4·max_cols` worst case. Rebuilt clean.

## Remaining gate — HARDWARE SIGN-OFF (not software-certifiable)
The SD write path is **not emulated** and EZ-Flash writes do not retry, so none
of the write ops are "done" until observed on real hardware. Run the consolidated
checklist: **[HARDWARE-SIGNOFF.md](HARDWARE-SIGNOFF.md)** (items B1–B50 on an
Omega DE; read-only check on an EverDrive — Trash items B45–B50 are Omega-only).
**Back up the microSD first** — for a permanent delete, that backup is the only
undo (with Delete mode = Trash, the recycle bin is the undo).

Open caveats tracked in the checklist: original (non-DE) Omega unproven; >2 TB
exFAT untested; RTC exposure is per-cart.
