# SD File Browser (Project A, Phases 0–3) — CONSOLIDATED MASTER HARDWARE SIGN-OFF

The gate for calling the whole tool **done**. Supersedes the per-phase list in
`HARDWARE-TEST-PHASE1.md`. SD log path is `/sdbrowse_log.txt` (card root, flushed
after every op). Until **B1–B18** are observed on a real **EZ-Flash Omega DE**
(and §8 on a real **EverDrive GBA X5**, plus the original-Omega caveat resolved
or explicitly accepted), Project A is **NOT "done"** regardless of build/emulator
status.

## What the emulator CANNOT prove (a green mGBA build proves none of this)
- The entire SD write path: `f_mkdir`/`f_unlink`/`f_chmod`/`f_rename`/`f_open(write)`
  → `_EZFO_writeSectors`. mGBA/melonDS no-op or fake-succeed it.
- **Writes have NO retry** (`io_ezfo.c`) — one transient hiccup is a hard failure;
  only the re-read verify catches it.
- FAT/FSINFO **free-space reclamation** after delete (must survive a power-cycle).
- Whether the **RTC is exposed** to this ROM (expect timestamps = 0 / "no timestamp").
- **OS-mode stability** across the thousands of back-to-back transfers a deep
  `rmtree` or a multi-MB recursive copy generates.
- mGBA debug logging is a no-op on hardware — the on-screen buffer and
  `/sdbrowse_log.txt` are the only evidence.

## Blocking items (each must be OBSERVED on a real Omega DE)
B1 mkdir survives remount · B2 single-file delete + space reclaimed across
power-cycle · B3 empty-folder delete · B4 non-empty recursive tree delete near
depth 24, space reclaimed after remount · B5 over-depth/length fails cleanly,
card stays mountable · B6 read-only file delete via clear-`AM_RDO` (incl. RO items
inside a tree) · B7 read-only + hidden toggles reflected after rescan · B8
rename/move survives remount, refuses clobber · B9 file copy byte-exact +
recursive folder copy byte-exact + cut = move (source gone) · B10 overwrite
safe-swap restores the original if paste fails · B11 multi-MB recursive copy
round-trips byte-exact · B12 hex edit: byte correct + rest intact + size preserved
+ `.bak~` = exact original · B13 forced verify-fail / disk-full leaves original
intact, card mountable · B14 rename-fail recovery: original recoverable from
`.bak~`/`.hexnew~` · B15 induced write failure reported (no hang/false success),
card still mounts · B16 `/sdbrowse_log.txt` written + readable, every destructive
op present · B17 RTC state recorded (plausible or "no timestamp", never a
fabricated date) · B18 EverDrive hides ALL write actions / is read-only.

---

## (0) Prep / back up
- [ ] Image the **whole microSD** off-card. The only undo for delete; the baseline for byte-diffs.
- [ ] Build the `.gba` (gbafix'd); note its hash. Confirm the cart is detected ("EZ-Flash Omega/DE").
- [ ] Record card format (FAT32 / exFAT) + free MB. **Flag any >2 TB exFAT** (`FF_LBA64 0`).
- [ ] Stage a disposable nested tree (many files, several levels; one branch near depth 24, one past 24),
      throwaway files of known PC-side content for the hex tests, and a large multi-MB file/folder.

## (1) Read-only — on BOTH carts
- [ ] Boots/mounts/browses; `[..]`, folders-before-files; log shows `SD mounted OK`.
- [ ] Nav (UP/DOWN repeat, LEFT/RIGHT jump 11, L/R page); all 6 sort states; free/total MB + `sel/total`.
- [ ] Detail panel (name/date/time/size); Properties (full wrapped name + attrs); Hex AND Text viewer on a file larger than RAM.
- [ ] Timestamps plausible OR "(no timestamp)" — never garbage. **Record which** → B17.

## (2) Creates / deletes incl. recursive + space reclaim — Omega DE
- [ ] New folder via OSK (A type / B del / L-R caret edit); OSK rejects empty, trailing space/dot, `.`/`..`, over-long. mkdir of existing → "already exists". → B1
- [ ] Delete single file (confirm; Cancel = no-op). → B2
- [ ] Delete empty folder. → B3
- [ ] Delete non-empty tree near depth 24 completes; past 24 fails cleanly, card mountable. → B4, B5
- [ ] **Power-cycle / remount → free space reclaimed** (record before/after/after-cycle). → B2, B4

## (3) Rename / move — Omega DE
- [ ] Rename file (OSK pre-filled) survives remount; rename folder works; rename to existing → "already exists". → B8

## (4) Copy / Cut / Paste + batch + overwrite safe-swap — Omega DE
- [ ] Copy file → Paste byte-exact; Cut → Paste = move (source gone); recursive folder copy byte-exact. → B9
- [ ] Paste folder into its own subtree → refused.
- [ ] Overwrite: prompt; on success old gone/new correct; **force paste to fail → moved-aside original restored**. → B10
- [ ] Multi-select: A marks (`*`), START all/clear, status count+size; batch Copy/Cut/Delete; B exits.
- [ ] **Multi-MB recursive copy** round-trips byte-exact; no hang. → B11

## (5) Attribute toggles — Omega DE
- [ ] Read-only ON (`R`) then delete that file (clear-`AM_RDO` path); delete a tree with RO items; toggle off. → B6
- [ ] Hidden ON/off (`H`); log lines. → B7

## (6) Hex editor — highest risk — Omega DE
Save = `<name>.hexnew~` → byte-verify → `<name>.bak~` backup → atomic rename. Size preserved.
- [ ] Enter EDIT (open → hex → START): white-box cursor, `[EDIT]` header.
- [ ] Edit one byte, Save (confirm). PC check: byte = new value, **rest identical, size unchanged, `.bak~` = exact original**. → B12
- [ ] Scattered edits (first/last byte, across pages) all land; undo (SELECT); B prompts discard.
- [ ] >512 distinct edits → "Edit buffer full" (no silent drop).
- [ ] **Forced verify-fail / disk-full** → "Save failed", original intact, card mountable. → B13
- [ ] **Rename-fail recovery** → "Save failed - RECOVER … name.bak~ / name.hexnew~"; bytes exist there. → B14
- [ ] Re-save twice: prior `.bak~` replaced; no stray `.hexnew~` after success.

## (7) Induced write-failure / no-retry — Omega DE
- [ ] Trigger a failed write → readable error (no hang/false success); card still mounts; code in log. → B15

## (8) EverDrive GBA X5 — read-only
- [ ] Actions menu shows ONLY Info/Properties + "Writes need EZ-Flash Omega"; no write items. → B18
- [ ] Hex viewer opens; **START does NOT enter EDIT** (read-only).

## (9) After EVERY run
- [ ] Read `/sdbrowse_log.txt` (every op + result). → B16
- [ ] PC mount: nothing outside targeted paths changed; byte-diff every written/edited file vs the backup.

---

## Open caveats (do NOT silently assume)
- **Original (non-DE) Omega: UNPROVEN.** Re-run the full flow; ROM wait-states **3,2 or slower**.
  The browser doesn't write SRAM (lower autosave-collision risk), but stays an open claim until run.
- **Delete has no verified-write/backup** (no `.tmp` for `f_unlink`/`rmtree`); the SD image (§0) is the
  sole undo — state this in the delete feature's user-facing warning.
- **>2 TB exFAT untested** (`FF_LBA64 0`); sub-2 TB expected fine.
- **RTC exposure is per-cart** — 0/"no timestamp" is expected, not a bug; never fabricate a date.
