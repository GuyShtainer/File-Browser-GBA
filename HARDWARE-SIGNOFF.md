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

---

# Phase 4 — Reboot-to-loader · Settings persistence · Themes · last-folder · show-hidden

Appended sign-off for the Phase-4 feature set. A clean mGBA build proves NONE of the
items below. New blocking items extend the master list: **B19–B30**. Until they are
OBSERVED on a real **EZ-Flash Omega DE** (and the EverDrive items on a real **EverDrive
GBA X5**), Phase 4 is **NOT "done"** regardless of build/emulator status.

Paths: config `/sdbrowse.cfg` (card root), log `/sdbrowse_log.txt` (card root).
Source anchors: reboot `lib/flashcartio.c` (`flashcartio_reboot`), `_EZFO_reboot`
`lib/ezflashomega/io_ezfo.c`, `ed_reboot` `lib/everdrivegbax5/everdrive.c`; cfg
`projects/sd-browser/source/cfg.c`; gating/flow `projects/sd-browser/source/main.c`
(`can_write`, `do_reboot`, `settings_menu`, startup load + last-dir reopen).

## New blocking items (each must be OBSERVED on real hardware)
B19 Omega DE: "Reboot to loader" lands somewhere RECORDED (kernel menu / tool restart /
black screen / power-cycle needed) — never an undefined hang · B20 EverDrive: same, on a
real X5 · B21 reboot reachable + confirm-gated from the idle menu, incl. on an empty root
(SELECT) · B22 `/sdbrowse.cfg` is written on Omega DE and survives a power-cycle · B23
every one of the 9 settings persists across a power-cycle (Omega DE) · B24 theme persists
across a power-cycle (Omega DE) · B25 last-folder reopen restores a still-existing dir ·
B26 last-folder FALLBACK: deleted/renamed last_dir → opens at root, card mountable · B27
show-hidden OFF hides AM_HID|AM_SYS, ON reveals them, after rescan · B28 confirm-delete OFF
deletes a THROWAWAY file with NO prompt (and the delete is still logged) · B29 EverDrive:
cfg_save never runs (write-gated) → settings RESET to defaults next launch, no `/sdbrowse.cfg`
created, no error · B30 jump distance + key-repeat min/max behave as set after a relaunch.

## (P4-SAFETY) Read this before the reboot tests
- [ ] **The reboot is EXPERIMENTAL and may not return to a usable state.** Have a way to
      **hard power-cycle** the console (and, for EverDrive, to re-enter its OS) ready BEFORE
      you press it.
- [ ] **Test on the throwaway card from §0 (no irreplaceable data).** Reboot writes no SRAM,
      but a wedged kernel hand-off can force a power-cycle mid-session.
- [ ] Reboot is invoked only while idle (`flashcartio_is_reading`-guarded) and forces
      `REG_IME=0` + IRQ/DMA/timer tear-down first. Do NOT trigger it during any copy/delete/hex-save.
- [ ] On the original (non-DE) Omega, keep the open caveat in mind: re-run with ROM
      wait-states **3,2 or slower** if you exercise reboot there too.

## (P4-1) Reboot to loader — Omega DE  → B19, B21
- [x] **CONFIRMED WORKING on hardware (user, 2026-06-03)** — "Reboot to loader" lands on the
      EZ-Flash loader menu. B19 PASS.
- [ ] Precondition: Omega DE, idle in the browser, card backed up.
- [ ] Steps: SELECT → actions menu → "Reboot to loader..." → confirm prompt reads
      "Reboot to loader?" / "Experimental - may just restart" → press A. Screen shows
      "Rebooting...".
- [ ] Expected: lands on the **EZ-Flash kernel/menu** (intended) OR documents the actual
      result. **Record EXACTLY one:** ☐ EZ-Flash menu ☐ tool restarted (back at browser root)
      ☐ black screen / hang ☐ required a manual power-cycle ☐ other: __________
- [ ] Confirm Cancel (B at the prompt) returns to the browser with NO reset.
- [ ] PASS / FAIL / NOTES (record landing spot verbatim): ______________________________

## (P4-2) Reboot to loader — EverDrive GBA X5  → B20, B21
- [ ] Precondition: EverDrive GBA X5, idle, card backed up.
- [ ] Steps: SELECT → "Reboot to loader..." → confirm → A. (Path: `ed_unlock_regs(); ed_reboot(0)`
      = swi 0x26 HardReset.)
- [ ] Expected: reaches the **EverDrive OS menu** (intended) OR documents actual result.
      **Record EXACTLY one:** ☐ EverDrive OS menu ☐ tool restarted ☐ black screen / hang
      ☐ required a manual power-cycle ☐ other: __________
- [ ] PASS / FAIL / NOTES (record landing spot verbatim): ______________________________

## (P4-3) Reboot reachable on an EMPTY root  → B21
- [ ] Precondition: card whose root directory is empty (no entries). EITHER cart.
- [ ] Steps: at the empty listing press **SELECT** → actions menu opens on the `[..]`/null row
      → confirm "Settings..." AND "Reboot to loader..." are both present and selectable.
- [ ] Expected: both reachable with no file selected (`actions_menu(NULL)`).
      Open Settings (no crash), back out; open Reboot → Cancel (do not reboot here).
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-4) Config file written + survives power-cycle — Omega DE  → B22
- [ ] Precondition: Omega DE; delete any existing `/sdbrowse.cfg` first (clean baseline).
- [ ] Steps: open Settings, change any value, press A (save). Power off fully, pull card,
      mount on PC.
- [ ] Expected: `/sdbrowse.cfg` exists, is human-readable INI (`[sdbrowse]` + key=value lines),
      contains the changed value. (Note: cfg_save is a **bare `f_write`, NOT verified-write** —
      see What-mGBA-cannot-prove; eyeball the file content.)
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-5) Every setting persists across a power-cycle — Omega DE  → B23, B24, B30
For each row: set to a non-default value in Settings, A to save, **fully power-cycle**, relaunch,
reopen Settings, confirm the value stuck. (Defaults: theme 0, sort name/asc, hidden off,
confirm-del ON, viewer Hex, jump 11, key delay 16, key speed 4, free MB.)
- [ ] Theme (try each of the 5; pick one non-default) persists → B24
- [ ] Sort key + direction persists
- [ ] Show hidden persists
- [ ] Confirm delete persists
- [ ] Open files (Hex/Text default viewer) persists
- [ ] L/R jump (set e.g. 25, range 1–99) persists → B30
- [ ] Key delay (set min 2, then max 60; range clamped) persists → B30
- [ ] Key repeat speed (set min 1, then max 30; range clamped) persists → B30
- [ ] Free-space unit (B/KB/MB/GB) persists and the header reflects it
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-6) Theme switch live + persistence — Omega DE  → B24
- [ ] Steps: Settings → cycle Theme through all 5 (4 dark + 1 light high-contrast); confirm
      each applies **live** (panel/text/border recolor immediately). Pick the light high-contrast
      one, A to save, power-cycle, relaunch.
- [ ] Expected: relaunch comes up in the chosen theme (read from `/sdbrowse.cfg`). Also confirm
      **B = cancel reverts** the live preview back to the saved theme.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-7) Last-folder reopen — happy path — Omega DE  → B25
- [ ] Steps: navigate into a nested folder (e.g. `/games/gba/`), open Settings + save (or
      reboot — both persist last_dir), then relaunch the tool.
- [ ] Expected: browser opens directly in `/games/gba/` (the saved `last_dir=`), not at root.
      Startup log line `cfg: ... last_dir='/games/gba'` present.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-8) Last-folder reopen — FALLBACK when last_dir is gone — Omega DE  → B26
- [ ] Steps: with `last_dir` pointing at `/games/gba/`, power off, on PC **delete or rename
      that folder**, return the card, relaunch.
- [ ] Expected: tool opens at **root `/`** (the `f_opendir` check fails), no hang, card fully
      browsable. (No dedicated log line for the fallback; confirm by the on-screen cwd being root.)
- [ ] Also: hand-edit `/sdbrowse.cfg` to a relative/garbage `last_dir` (no leading `/`) →
      expect root (the `last_dir[0]=='/'` guard rejects it).
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-9) Show-hidden toggle — Omega DE  → B27
- [ ] Precondition: stage at least one file/folder with the Hidden (and one with System)
      attribute set (set Hidden in-tool via the H action, or PC-side).
- [ ] Steps: Settings → Show hidden = off → save (listing rescans). Then = ON → save.
- [ ] Expected: with OFF, AM_HID|AM_SYS entries are **absent**; with ON they appear
      (`fsop_list(..., show_hidden)`). `sel/total` count changes accordingly.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-10) Confirm-delete OFF — deletes with NO prompt — Omega DE  → B28
- [ ] **Precondition: a THROWAWAY file you can lose** (created for this test on the backed-up card).
- [ ] Steps: Settings → Confirm del = off → save. Select the throwaway file → actions → Delete.
- [ ] Expected: file is deleted **immediately with no confirmation prompt** (gate on
      `g_set.confirm_delete`). The delete is still recorded in `/sdbrowse_log.txt`. Then set
      Confirm del = ON and verify the prompt returns.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-11) Jump distance + key-repeat feel — Omega DE (or EverDrive read-only)  → B30
- [ ] Set L/R jump to a small value (e.g. 3) and a large value (e.g. 40); confirm LEFT/RIGHT
      moves the selection by exactly that many rows (clamped at list ends).
- [ ] Set key delay = min 2 + key speed = min 1 → held UP/DOWN should feel near-instant/fast.
      Set delay = max 60 + speed = max 30 → noticeably slow initial hold + slow repeat.
- [ ] Confirm the tuning takes effect immediately on save (`key_repeat_limits`) AND after a
      relaunch (loaded from cfg).
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-12) EverDrive cfg-save is a harmless NO-OP — EverDrive GBA X5  → B29
- [ ] Precondition: EverDrive X5; ensure NO `/sdbrowse.cfg` on the card to start.
- [ ] Steps: open Settings, change theme + several values, press A (save). Note: on EverDrive
      `can_write()` is false, so **cfg_save is never even called** — the write is skipped
      entirely, not attempted-and-failed. Power-cycle, relaunch.
- [ ] Expected: NO error/hang on save; on relaunch **settings are back to DEFAULTS**; pull the
      card on PC and confirm **`/sdbrowse.cfg` was NOT created**. (Reads still work on both carts:
      a cfg from an Omega session will load but cannot be updated here.)
- [ ] **Verify the claim explicitly:** do EverDrive settings persist? Record the observed answer:
      ☐ reset to defaults (expected) ☐ unexpectedly persisted ☐ other: ______
- [ ] PASS / FAIL / NOTES: ______________________________

## (P4-after) After every Phase-4 run
- [ ] Read `/sdbrowse_log.txt`: the startup `cfg: theme=… last_dir='…'` line is present and
      matches what you set. **Note:** Settings-menu saves and the Reboot action are NOT
      individually logged (only the startup cfg line + destructive file ops are).
- [ ] PC-mount: confirm only `/sdbrowse.cfg` (Omega) and `/sdbrowse_log.txt` changed; no other
      files moved. On EverDrive, confirm `/sdbrowse.cfg` was not created by the tool.

---

## What mGBA CANNOT prove (Phase 4)
A green mGBA build proves none of the following:
- **The `/sdbrowse.cfg` write path** — `cfg_save` → `f_open(FA_CREATE_ALWAYS|FA_WRITE)` →
  `f_write` → `_EZFO_writeSectors`. mGBA/melonDS no-op or fake-succeed every SD write, so
  "settings saved" in the emulator means nothing. **Note also this write is a bare `f_write`,
  NOT the toolkit's verified-write (`.tmp` → re-read → rename) pattern, and EZFO writes have no
  retry** — a transient hiccup silently loses the save. Acceptable for a non-critical config
  (defaults next launch), but unverified on hardware until B22 passes.
- **The rompage / reset behavior** — `SetRompage(ROMPAGE_BOOTLOADER)` + `swi 0x00` SoftReset on
  Omega and `swi 0x26` HardReset on EverDrive. mGBA does not model the EZFO bootloader page swap
  or the cart kernel, so where the reboot actually LANDS is unobservable in emulation — the
  single biggest unknown in Phase 4, hence B19/B20 must be recorded by hand.
- **Real cart kernel/OS interaction** — whether the EZ-Flash kernel menu and the EverDrive OS
  menu actually receive control (vs. re-launching the same ROM, hanging, or needing a
  power-cycle) is a property of the physical cart firmware, not emulated at all.
- **Free-space / FAT effects of the cfg + delete** — whether `/sdbrowse.cfg` and the
  confirm-delete-OFF deletion actually commit and survive a power-cycle is invisible to mGBA.
- **RTC-dated cfg writes** — `/sdbrowse.cfg`'s mtime comes from `get_fattime()`; if the RTC is
  not exposed to this ROM the timestamp will be 0/epoch (expected, per B17). Verify "saved" by
  file content, not timestamp.
- mGBA debug logging is a no-op on hardware — the on-screen UI and `/sdbrowse_log.txt` are the
  only evidence; and the Settings/Reboot actions are not logged at all.

---

# Phase 5 — START/SELECT swap · scrolling actions menu · 6 new ops

Appended for the Phase-5 batch. New blocking items: **B31–B38**. The three write
ops (Duplicate, New file, Swap names) are not "done" until OBSERVED on a real
**EZ-Flash Omega DE**; the read-only ones (Folder size, Go to offset) and the UI
changes (swap, scroll, Reset) can be confirmed on either cart.

Source anchors (all `projects/sd-browser/source/`): swap + scroll in `main.c`
(`run_browser`, `actions_menu`); `do_duplicate`/`duplicate_path`, `do_newfile`,
`do_foldersize`, `do_swap_names`, `parse_hex64`; `fsop_dirsize` in `fs_ops.c`.

## New blocking items
B31 START opens the viewer/actions menu and SELECT cycles sort (browser); START
opens the batch menu and SELECT marks-all (selection mode) — and the keyboard,
hex-editor save, settings save still use START · B32 actions menu SCROLLS with
▲/▼ when items exceed the window; no row ever spills past the box or onto the
footer (test a folder on Omega with a full clipboard = 15 items) · B33 Duplicate
makes a correctly-named sibling copy of a file AND a folder (recursive), never
overwriting an existing entry · B34 New file creates an empty file (FR_EXIST on a
name clash, no clobber) · B35 Swap names exchanges two marked items' names (mark
exactly 2 → "Swap names"); on an induced failure neither item is lost/duplicated
· B36 Folder size totals bytes + counts on a deep/wide tree, on BOTH carts · B37
Go to offset jumps the viewer to a typed hex offset (clamped to file size) · B38
Reset to defaults restores prefs (keeps last folder); B in settings still cancels ·
B39 Find: keyword search lists matches across the subtree and "go to" lands in the
match's folder with it selected — on BOTH carts.

## (P5-1) START/SELECT swap — either cart  → B31
- [ ] Browser: **START** on a file opens the Hex/Text viewer; **START** on a folder/`[..]`/
      empty dir opens the actions menu; **SELECT** cycles the 6 sort states (status bar updates).
- [ ] Selection mode ("Select multiple"): **A** marks; **SELECT** marks-all/clears-all;
      **START** opens the batch menu; **B** exits. Footer reads "A mark SE all ST batch B exit".
- [ ] Unchanged: on-screen keyboard **START**=confirm / **SELECT**=cancel; hex editor
      **START**=save / **SELECT**=undo; settings **A/START**=save. Confirm these did NOT swap.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P5-2) Scrolling actions menu  → B32
- [ ] On Omega, select a **folder**, Copy something first (so Paste shows), then open the
      actions menu (15 items). Scroll with UP/DOWN: a ▲ shows when more is above, ▼ when more
      below; the selection never draws past the panel box or over the "A do  B back" footer.
- [ ] On EverDrive, the menu (Info/Folder size/Settings/Reboot) fits with no carets.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P5-3) Duplicate — Omega DE  → B33  (write)
- [ ] Duplicate a **file** `foo.txt` → creates `foo (copy).txt`; duplicate again → `foo (copy 2).txt`.
- [ ] Duplicate a **folder** `bar` (with contents) → creates `bar (copy)` recursively; verify the
      copy's bytes match (byte-diff on PC). A folder with a dot (`my.backup`) → `my.backup (copy)`
      (suffix at the END, not mid-name).
- [ ] Confirm an existing `foo (copy).txt` is never overwritten (it skips to the next free name).
- [ ] PASS / FAIL / NOTES: ______________________________

## (P5-4) New file — Omega DE  → B34  (write)
- [ ] "New file here" → type a name → a 0-byte file appears; open it in the hex viewer (empty).
- [ ] Re-create the same name → "Create failed / already exists" (FR_EXIST), original untouched.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P5-5) Swap names — Omega DE  → B35  (write)
- [ ] Select multiple → mark **exactly two** files `A.txt`, `B.txt` → START → "Swap names".
      After: `A.txt` holds old B's content, `B.txt` holds old A's. (Verify by content on PC.)
- [ ] "Swap names" is **absent** when 1 or 3+ are marked.
- [ ] Mark two **folders** → swap → both directories' contents follow their new names.
- [ ] No `*.sdbtmp~` residue is left behind on success (PC-mount check).
- [ ] PASS / FAIL / NOTES: ______________________________

## (P5-6) Folder size — both carts  → B36  (read-only)
- [ ] Actions → "Folder size" on a folder with nested subfolders → "Computing size..." then a
      result screen with total size + "N files, M folders". Spot-check against the PC's reported size.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P5-7) Go to offset — both carts  → B37  (read-only)
- [ ] In the viewer press **SELECT** → type a hex offset (e.g. `1000`) → the window jumps there
      (offset label matches). Offsets past EOF clamp to the last page. Cancel (B) leaves it put.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P5-8) Reset to defaults — Omega DE  → B38
- [ ] Change several settings + theme, scroll to "Reset to defaults", press **L/R** → all rows
      revert to defaults live; the **remembered last folder is kept**. Press **A** to save (or **B**
      to cancel the reset). Power-cycle and confirm the reset persisted (when saved with A).
- [ ] PASS / FAIL / NOTES: ______________________________

## (P5-9) Find — recursive keyword search — both carts  → B39  (read-only)
- [ ] In a folder with nested subfolders, actions → "Find..." → type a keyword that matches
      several names → a scrollable result list appears (▲/▼ when long; "N+" if capped at 128).
- [ ] Verify matches are correct (substring, case-insensitive) and include items in subfolders.
- [ ] "Go to" (A/START) a result → the browser opens that item's folder with it highlighted;
      a result in the current folder stays put + highlights. B returns with no change.
- [ ] Searching a large tree shows "Searching..." then results (no hang); a keyword with no
      match shows "No matches".
- [ ] Note: find does NOT filter hidden — a match that is Hidden/System won't be highlighted
      after navigation unless "Show hidden" is on (you still land in its folder). Confirm this
      is acceptable.
- [ ] PASS / FAIL / NOTES: ______________________________

## What mGBA cannot prove (Phase 5)
- The three write ops (Duplicate / New file / Swap names) go through the real SD write path
  (`fsop_copy` / `f_open(FA_CREATE_NEW)` / `f_rename`), which the emulator fakes — verify on
  hardware that bytes actually land and survive a power-cycle.
- `do_swap_names` rollback on a mid-swap failure is hard to provoke; the review verified the
  logic by inspection, but the real-hardware failure path (e.g. SD yank) is unobservable in mGBA.
- `fsop_dirsize` on a genuinely deep tree (near FS_RMTREE_MAX_DEPTH=24) is only meaningful on a
  real card's directory structure.

---

# Phase 5 addendum — keyboard auto-repeat · START=menu · View-in-menu

UI refinements (no SD/RTC/destructive change). New items **B40–B41**; both confirm
on either cart / mGBA.

B40 keyboard auto-repeat · B41 START opens the menu + View lives in it.

## (P5-10) Keyboard auto-repeat  → B40
- [ ] In any rename/new-file/find prompt, **hold** a d-pad direction → the character cursor
      slides continuously; **hold L/R** → the text caret slides. Speed matches the Settings
      "Key delay / Key repeat" values (change them and re-confirm the feel).
- [ ] A (insert) does NOT auto-repeat on hold (one char per press); B, START, SELECT discrete.
- [ ] Open "Go to offset" from the viewer, then return — the viewer's own L/R paging auto-repeat
      still works (the repeat mask was saved/restored, not clobbered).
- [ ] PASS / FAIL / NOTES: ______________________________

## (P5-11) START = actions menu · View in the menu  → B41
- [ ] **START** on a file opens the actions menu (NOT the viewer directly); the menu's first
      item is **"View (hex/text)"**. START on a folder/`[..]`/empty dir also opens the menu.
- [ ] A on a file opens the same menu; A on a folder enters it; A on `[..]` goes up. SELECT sorts.
- [ ] "View" opens the hex/text viewer; after a hex-edit+save the listing refreshes (date), after
      a pure view it does not. "View" is absent on folders/`[..]`.
- [ ] PASS / FAIL / NOTES: ______________________________

---

# Phase 6 — file-type viewers (dispatcher · word-wrap text · BMP)

UI/read-only viewers (no SD write, no RTC, no destructive op). New items **B42–B44**;
all confirm on either cart / mGBA. (Future P6d/e metadata + P6f image codecs are not
built yet.)

B42 open-by-extension dispatch · B43 BMP image viewer · B44 word-wrapped text.

## (P6-1) Open-by-extension dispatch  → B42
- [ ] On a `.bmp` file the actions menu shows an **"Open" row** (labelled "View image")
      ABOVE "View (hex/text)"; on a non-registered type only "View (hex/text)" appears.
- [ ] "View (hex/text)" is ALWAYS present for files (the universal fallback). Folders show no View/Open.
- [ ] PASS / FAIL / NOTES: ______________________________

## (P6-2) BMP image viewer — both carts  → B43
- [ ] Stage BMP files on the card: a 24-bit, an 8-bit palettized, and (if easy) a 16-bit and a
      32-bit — at sizes both ≤240×160 and larger (to exercise downscale). "View image" on each:
      the picture renders centered, correct colors, correctly oriented (not upside-down), with a
      "name WxH B=back" footer. **B** returns to the browser (image cleared).
- [ ] A NON-BMP renamed to `.bmp`, a truncated BMP, and an RLE-compressed BMP each show a clear
      message ("Not a BMP" / "Compressed BMP" / "Unsupported"/"too wide") and never hang/garble.
- [ ] Confirm it works on **EverDrive** too (read-only path).
- [ ] PASS / FAIL / NOTES: ______________________________

## (P6-3) Word-wrapped text viewer  → B44
- [ ] Open a `.txt`/`.c`/`.md` with long lines, tabs, and CRLF line endings (View → toggle to
      Text with A): lines wrap at word boundaries (not mid-word), tabs align to 4-col stops, no
      stray `.` for CR, and paragraph breaks render. High/UTF-8 bytes show as `.` (ASCII font).
- [ ] L/R page + UP/DOWN scroll still work (note: a page boundary may split a line — expected).
- [ ] PASS / FAIL / NOTES: ______________________________
