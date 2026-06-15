# File-Browser-GBA — release status

**Status: HARDWARE-VALIDATED on a Game Boy Advance SP + EZ-Flash Omega DE — the
whole tool, including the recycle bin (Trash), works on the real cartridge.
Builds clean (zero warnings).**

`file_browser_gba.gba` — ROM ~130 KB, IWRAM ~11.9 KB / 32 KB, EWRAM well within 256 KB.

**Self-contained:** the shared hardware/FS layer (flashcartio, FatFs, the
per-cart block drivers, the cartridge RTC and the logger) is vendored into
`lib/` + `source/`, so the repo builds standalone (`./build.sh`, or `make
rebuild` with a local devkitARM) with no external checkout.

## Validated on real hardware (GBA SP + EZ-Flash Omega DE)
- **Tested on the actual cartridge — there were ZERO emulator runs.** Every read
  AND write feature (P0–P7) has been exercised on a Game Boy Advance SP with an
  EZ-Flash Omega DE and works: browsing / sort / search / properties / free-space
  / viewers, new file+folder, rename/move, copy/cut/paste (incl. recursive folder
  copy), duplicate, delete, attribute toggles, the verified hex editor, settings
  persistence + last-folder + themes, reboot-to-loader, and the **recycle bin
  (Trash): move-to-trash, restore, delete-forever, empty**. The EverDrive GBA X5
  runs read-only.
- **New since validation: auto-clear old trash** (Settings → *Auto-clear*, Off by
  default, 1–365 days). Purges trashed items older than N days at launch, dated
  from the origin sidecar (cart RTC). Opt-in and fails safe — does nothing if Off
  or the RTC isn't readable. (This new code path is the only thing added after the
  hardware pass; worth a quick check on the cart.)
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

## No remaining gate — only watch the new auto-clear option
The full feature set (P0–P7, including Trash) has been observed on a real
EZ-Flash Omega DE. The only code added *after* that hardware pass is the opt-in
**Auto-clear old trash** option — it fails safe (does nothing if Off or the RTC
is unreadable), but since it deletes automatically, give it a quick check on the
cart with a short day count (items B51 in
[HARDWARE-SIGNOFF.md](HARDWARE-SIGNOFF.md)). **Back up the microSD before bulk
deletes** as a general habit — EZ-Flash writes don't retry.

Open caveats: original (non-DE) Omega unproven; >2 TB exFAT untested; RTC
exposure is per-cart (auto-clear needs the RTC).
