# File-Browser-GBA — release status

**Status: HARDWARE-VALIDATED on a Game Boy Advance SP + EZ-Flash Omega DE.
Everything works on the real cartridge EXCEPT the brand-new recycle bin (Trash),
which is the only feature not yet hardware-tested. Builds clean (zero warnings).**

`file_browser_gba.gba` — ROM ~130 KB, IWRAM ~11.9 KB / 32 KB, EWRAM well within 256 KB.

**Self-contained:** the shared hardware/FS layer (flashcartio, FatFs, the
per-cart block drivers, the cartridge RTC and the logger) is vendored into
`lib/` + `source/`, so the repo builds standalone (`./build.sh`, or `make
rebuild` with a local devkitARM) with no external checkout.

## Validated on real hardware (GBA SP + EZ-Flash Omega DE)
- **Tested on the actual cartridge — there were ZERO emulator runs.** Every read
  AND write feature in P0–P6 has been exercised on a Game Boy Advance SP with an
  EZ-Flash Omega DE and works: browsing / sort / search / properties / free-space
  / viewers, new file+folder, rename/move, copy/cut/paste (incl. recursive folder
  copy), duplicate, delete, attribute toggles, the verified hex editor, settings
  persistence + last-folder + themes, and reboot-to-loader. The EverDrive GBA X5
  runs read-only.
- **Only the new recycle bin (Trash, P7) is NOT yet hardware-tested** — see the
  Remaining gate below. Because *Delete mode = Trash* is the default, the default
  delete path is the one open item; *Delete mode = Permanent* is the long-proven
  behaviour.
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

## Remaining gate — only the recycle bin (Trash)
Everything else has been observed on real hardware. The **Trash** feature (P7,
items **B45–B50** in [HARDWARE-SIGNOFF.md](HARDWARE-SIGNOFF.md)) is the one path
that has not been run on the cartridge yet. The SD write path is not emulated and
EZ-Flash writes do not retry, so Trash isn't called "done" until a real Omega DE
move/restore/empty is observed. Tracked in
[issue #1](https://github.com/GuyShtainer/File-Browser-GBA/issues/1).
**Back up the microSD before bulk Trash testing** — or use *Delete mode =
Permanent* for the long-proven delete.

Open caveats: original (non-DE) Omega unproven; >2 TB exFAT untested; RTC
exposure is per-cart.
