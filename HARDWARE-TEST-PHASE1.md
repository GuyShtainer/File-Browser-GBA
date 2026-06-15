# Phase 1 hardware sign-off checklist (first WRITE phase)

> **Superseded by [HARDWARE-SIGNOFF.md](HARDWARE-SIGNOFF.md)** — the consolidated
> master checklist covering all of Phases 0–3. This file is kept for history.

Per CLAUDE.md convention 7, Phase 1 (mkdir / delete / attributes) is **not "done"
until validated on real hardware** — the SD write path is invisible to emulators
(it no-ops or fake-succeeds), so a clean compile + code review prove nothing about
writes. Phase 0 (read-only) already ran on hardware; that does **not** transfer to
writes.

## Critical facts before you start

- **EZ-Flash writes have NO retry** (`io_ezfo.c`): one transient hiccup = hard failure.
- **Delete has no undo and no `.tmp` recovery** — `f_unlink`/`rmtree` destroy data
  immediately. **The SD card backup (step 0) is the only undo.**
- Every op is logged to **`/file_browser_gba_log.txt`** at the card root (flushed after each op).
  Pull the card and read it after every run. mGBA debug logging is a no-op on hardware.

## Procedure (EZ-Flash Omega DE)

- [ ] **0. Back up the whole microSD first** (full image). Make a **disposable nested
      folder tree** for destructive tests. Note the card's format (FAT32 / exFAT).
- [ ] **1. Read-only regression:** boots, mounts, browses, shows free/total MB; cart
      detected as Omega; `/file_browser_gba_log.txt` shows `SD mounted OK`.
- [ ] **2. mkdir + keyboard:** SELECT → New folder; type via the OSK (A type, B backspace,
      mixed case/digits/`_`/`-`). Verify OSK **rejects** empty, trailing space, trailing
      dot, `.`/`..`, and over-long names (warning bar, START won't dismiss). Folder appears
      after rescan. Info shows a plausible date **or** "(no timestamp)" (never garbage) —
      record which (tells you if the RTC is exposed). `mkdir` of an existing name → "already exists".
- [ ] **3. Delete a single file** (disposable): confirm prompt appears; Cancel does nothing;
      Confirm removes it. Log shows `delete ... (dir=0) -> 0 (OK)`.
- [ ] **4. Delete an EMPTY folder.**
- [ ] **5. Delete a NON-EMPTY nested tree** (the headline test of `rmtree`): many files,
      several levels. Include one branch **near depth 24** (completes) and one **past 24**
      (fails cleanly: "name too long / too deep", card stays mountable). Verify the tree is
      fully gone, then **power-cycle / remount and confirm free space was reclaimed**
      (catches FAT/FSINFO desync). Record free MB before / after / after-power-cycle.
- [ ] **6. Read-only path:** toggle Read-only ON (attr column shows `R`), then **delete that
      read-only file** (exercises clear-AM_RDO-then-unlink). Also delete a tree that
      *contains* read-only items. Toggle back off.
- [ ] **7. Hidden toggle:** toggle Hidden ON/off (attr column shows `H`); confirm log lines.
- [ ] **7b. Rename (Phase 2):** SELECT → Rename; the keyboard pre-fills the current name;
      edit it and confirm. Entry shows the new name after rescan. Rename to an **existing**
      name → "already exists" (no clobber). Rename a folder works. Log shows
      `rename <old> -> <new> -> 0 (OK)`. (Lower risk: touches only directory-entry sectors,
      no file-data rewrite — but EZ-Flash writes don't retry, so verify the entry survives.)
- [ ] **8. Card format:** confirm your card's format and run mkdir + delete on it. If
      possible test one FAT32 **and** one exFAT card. **Only real risk:** a **>2 TB exFAT**
      card (config is `FF_LBA64 0`) — avoid or test explicitly. Sub-2 TB is expected fine.
- [ ] **9. Induced write failure:** trigger a failed op (e.g. mkdir over an existing name, or
      over-deep delete). Confirm an error screen with a readable reason appears (no hang, no
      false success) and the card **still mounts** afterward (no corruption). Failing code is
      in `/file_browser_gba_log.txt`.
- [ ] **10. EverDrive GBA X5:** boot the same ROM; confirm **all write actions are absent**
      (only "Info / properties" + the "Writes need EZ-Flash Omega" hint). Tool is read-only.
- [ ] **11. After every run:** read `/file_browser_gba_log.txt`; mount the card on a PC and confirm
      nothing **outside** the targeted paths changed.

## Emulator CANNOT prove (must be hardware)

The entire SD write path (`f_mkdir`/`f_unlink`/`f_chmod` → `_EZFO_writeSectors`); the
no-retry timeout behavior; FAT/FSINFO free-space reclamation after delete; whether the
RTC is exposed to this ROM; OS-mode stability across the many back-to-back transfers a
deep `rmtree` generates.

## Blocking "done" items (each must be observed on real Omega DE)

1. mkdir survives remount (typed name).  2. Single-file delete; space reclaimed across
power-cycle.  3. Empty-folder delete.  4. Non-empty tree delete near depth 24 + **free
space reclaimed after remount**.  5. Over-depth/length fails cleanly, card stays mountable.
6. Read-only file delete via clear-AM_RDO path (incl. read-only items inside a tree).
7. Read-only + Hidden toggles reflected after rescan.  8. Induced write failure reported,
card uncorrupted (no-retry path handled).  9. `/file_browser_gba_log.txt` written + readable.
10. EverDrive hides writes / read-only.  11. mkdir+delete on the actual card format
(>2 TB exFAT flagged).  12. RTC state recorded (plausible or "(no timestamp)", no fabricated dates).

## Open items (not on the Omega DE path)
- **Original (non-DE) Omega** not yet covered (SRAM-autosave collision; wait-states 3,2+).
  The browser doesn't write SRAM so risk is lower, but "works on original Omega" stays an
  **unproven claim** until run there.
- **Delete bypasses the verified-write/backup pipeline by design** (no `.tmp` for `f_unlink`);
  the card backup is the sole undo — state this in the delete feature's user-facing warning.

---

# Phase 3 — hex editor (highest risk: edits arbitrary bytes of any file)

Save is a verified rewrite: write `<name>.hexnew~` = original-with-edits, **byte-verify**
it, back the original up to `<name>.bak~`, then atomically rename the temp into place
(file size preserved; no insert/append). Reviewed safe-to-commit, but the SD write path is
real/no-retry and unemulated, so it needs sign-off on a real Omega DE.

- [ ] **Back up the SD first.** Pick a **throwaway file** (note its full byte content on a PC).
- [ ] **Enter EDIT:** open it (SELECT) → hex → START. Confirm a **white-box cursor** appears
      and the `[EDIT]` header shows; on EverDrive confirm START says "read-only" (no edit).
- [ ] **Edit a byte:** move the cursor, L/R change the value (edited byte highlights). Save
      (START → confirm). Then on a PC: the **one byte changed to the new value**, **every
      other byte is identical**, the **file size is unchanged**, and **`<name>.bak~` holds the
      exact pre-edit original**.
- [ ] **Multiple + scattered edits** (incl. first byte, last byte, across page boundaries):
      all land; unedited bytes intact.
- [ ] **Undo (SELECT)** clears pending edits; **B with unsaved edits** prompts discard.
- [ ] **Edit-buffer cap:** make >512 distinct-byte edits → "Edit buffer full" appears (no
      silent drop); after saving/undo you can edit again.
- [ ] **Induced failure — near-full card:** edit a large-ish file with little free space so the
      temp write fails; confirm **"Save failed"**, the **original is intact** (verify on PC),
      and the card stays mountable. (Optional, harder: force the final rename to fail and
      confirm the "RECOVER … name.bak~ / name.hexnew~" message + that the bytes exist there.)
- [ ] **Re-save** the same file twice; confirm the prior `.bak~` is replaced (single-slot) and
      no stray `.hexnew~` remains after a successful save.
- [ ] **`/file_browser_gba_log.txt`** shows the `hexedit … -> 0 (OK)` line.

**Emulator cannot prove:** the whole edit→verify→backup→swap write path (no-retry EZ-Flash
writes + renames). **Blocking "done" items:** (1) edited byte correct + rest intact + size
preserved + `.bak~` = original, on real hardware; (2) a forced **verify-fail / disk-full**
leaves the original intact and the card mountable; (3) the **rename-fail recovery** path
(original recoverable from `.bak~`/`.hexnew~`); (4) EverDrive cannot enter EDIT mode.
