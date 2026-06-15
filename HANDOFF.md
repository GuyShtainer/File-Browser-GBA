# SD File Browser (Project A) — Handoff

> Living resume doc maintained by the `handoff` skill. The **Current status** and **Next steps**
> sections are always kept current — start there to resume. The **Session log** grows downward,
> newest first, and is never pruned.
> Last updated: 2026-06-10

## Current status

- **Repo / branch:** `/Users/guyshtainer/VSCodeProjects/gba-toolkit/projects/sd-browser` / `main` — pushed: **no** (no remote yet)
- **Goal:** A cartridge-native microSD **file manager** for the GBA running on EZ-Flash Omega DE + EverDrive GBA X5, built on the gba-toolkit shared layer. Reads on **both** carts; all **writes are Omega-only**.
- **State right now:** **Code-complete P0–P6c, builds clean** (zero warnings; ROM ~0.37% of 32 MB, IWRAM ~37% of 32 KB, EWRAM ~0.57%). Everything is **committed** (`main`, not pushed). The only thing between here and "done" is the **real-hardware sign-off** — most write/edit + new-viewer features have not been validated on a cart yet.
- **Done** (all committed):
  - **P0** browse / sort (name·size·date ×asc/desc) / properties / free-space / hex+text viewer — `a` earlier commits; **HW-validated on Omega DE**.
  - **P1** on-screen QWERTY keyboard + mkdir / delete (recursive, explicit-stack) / attribute toggles — Omega-only.
  - **P2** rename·move / copy·cut·paste (recursive folders, overwrite safe-swap) / multi-select batch / editable-caret keyboard.
  - **P3** in-place hex editor (verified write: `.tmp`→byte-verify→`.bak~`→atomic rename).
  - **P4** settings menu + **5 themes** + `/sdbrowse.cfg` persistence + last-folder reopen + show-hidden + **reboot-to-loader** — commit `61783e8`. *Reboot HW-confirmed on Omega DE (lands on the EZ-Flash kernel menu).*
  - **P5** START⇄SELECT swap · scrolling actions menu · folder-size · duplicate · new-file · swap-names · go-to-offset · reset-settings · recursive keyword **find** · keyboard auto-repeat — commit `61783e8`.
  - **P6a/b/c** open-by-extension **dispatcher** + **word-wrap text** viewer + **BMP image viewer** — commit `7b58a1a`.
  - Shared-lib **reboot** (`flashcartio_reboot` / `_EZFO_reboot`) lives in the **foundation repo** (`gba-toolkit`), commit `31dab18` — not in this repo (pulled via `../../lib`).
- **In progress:** nothing mid-edit — feature-complete for the planned scope.
- **Blocked / needs the user:**
  1. **Real-hardware sign-off** — the SD write path, RTC, and rompage/reset are **not emulated**, so a clean build proves none of them. Checklist = **B1–B44** in `HARDWARE-SIGNOFF.md`. Only **P0 browse** + **reboot-to-loader** (Omega DE) are confirmed; **P1+ writes/edits and all P5/P6 viewers are sign-off-pending.**
  2. **GitHub repo** decisions (see Next steps #2) — deferred by the user to *after* testing.

## Next steps (resume here)

1. **Hardware sign-off** (back up / image the SD card first — writes can lose data, EZ-Flash writes don't retry). Work `HARDWARE-SIGNOFF.md`; if time-boxed, do **Tier 1 destructive writes** (B1–B15: mkdir/delete/free-space-reclaim, rename, copy/cut/paste + overwrite safe-swap + multi-MB, attrs, hex-edit save/verify-fail/recover; **B33–B35** duplicate/new-file/swap-names) and **Tier 3 new viewers** (B42–B44 dispatcher/BMP/word-wrap; B36–B39 folder-size/go-to-offset/find). Then **B22–B30** settings persistence, **B18/B29/B20** EverDrive read-only posture. Report PASS/FAIL → fold into `HARDWARE-SIGNOFF.md`.
2. **Create the public GitHub repo** (the user's stated follow-up). Prereqs — **it is not a one-command push:**
   - **Vendor the shared layer.** This repo references `../../lib`, `../../source` via the Makefile, so it **won't build standalone**. Copy in `lib/` + `source/{gba_rtc,log}` to make it self-contained (precedent: `projects/gba-pokeviewer` did exactly this).
   - **License + third-party review.** No `LICENSE` yet. The vendored `lib/` includes code adapted from the EZ-Flash kernel, the EverDrive driver, FatFs, and links tonc — run a `gba-toolkit/docs/kb/licensing.md` review **before** publishing; pick the tool's license (gba-pokeviewer chose **MIT**).
   - **Push.** `gh` CLI is **not installed** here → either install it or create an empty repo on github.com, then `git remote add` + `git push`. **Publishing is public + hard to undo — confirm with the user before pushing.**
3. **Optional future viewers** (planned in `gba-toolkit/docs/ROADMAP.md` §A Phase 6, not built): **P6d** media-metadata inspector (ID3 / MP4 `moov` / WAV), **P6e** PDF info panel, **P6f** stretch image codecs (PNG/GIF/JPEG) + an EWRAM-resident WAV/ADPCM short-clip player. Real **mp3/mp4/pdf playback/rendering is INFEASIBLE** — these are metadata/info views only.

## How to build / test / run

```sh
# local devkitARM (env vars are NOT in the login shell; absolute -C path)
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM \
PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH \
make -C /Users/guyshtainer/VSCodeProjects/gba-toolkit/projects/sd-browser rebuild
# (./build.sh = Docker path, but Docker is absent on this machine)
```
Output: `sd_browser.gba` (ROM title `SDBROWSE`). Copy to the flashcart microSD and launch like any GBA ROM. No host-test harness is wired for sd-browser; pure-C cores (`fsop_*`, `contains_ci`) were validated ad-hoc on the PC.

## Key decisions (and why)

- **Reads both carts, writes Omega-only** — gated on `can_write() == EZ_FLASH_OMEGA`; EverDrive write isn't wired in the shared layer.
- **START = "open the menu" everywhere; SELECT = cycle sort** (user request). The hex/text viewer moved *into* the actions menu as **"View (hex/text)"**; an **open-by-extension dispatcher** adds a type row above it (BMP → "View image"), with View always the fallback. START stays "confirm/save" in the keyboard/hex-save/settings so it's the consistent primary button.
- **Config is a bare `f_write`** (not the verified-write pipeline) — acceptable for a non-critical, regenerable settings file; write Omega-only, read on both.
- **Reboot-to-loader** = `SetRompage(BOOTLOADER)` + `swi 0x00`, from an EWRAM-resident function (ROM unmaps the instant the rompage flips) → reaches the EZ-Flash kernel menu (**HW-confirmed**). EverDrive = `ed_unlock_regs()` (mandatory — regs locked after activate) + `ed_reboot(0)` (experimental).
- **Media feasibility:** real MP3/AAC, MP4/H.264 video, PDF rendering are **hardware-infeasible** (16.78 MHz, no FPU, + OS-mode SD rule) → offer metadata/info + hex/text; images are feasible (BMP built). Full rationale: ROADMAP §A Phase 6.

## Where things live

- `source/main.c` — UI/state machine: `run_browser`, `actions_menu` (the `A_*` enum incl. `A_OPEN`/`A_VIEW`/`A_FIND`, scrolling window), `settings_menu`, `find_modal`+`apply_find_sel`, `file_viewer`, `view_image` (BMP→Mode-3), `render_text_wrapped`, `opener_for`/`g_openers[]` registry, the `do_*` write ops, `do_reboot`.
- `source/fs_ops.c` / `.h` — pure-C file ops: `fsop_list`/`sort`/`freespace`/`mkdir`/`rename`/`chmod`/`copy`/`delete`/`apply_edits`/`dirsize`/`find` + `fsop_ext`/`fsop_ext_is` (one shared **explicit dir-stack**, no C recursion; host-testable).
- `source/cfg.*` (INI settings), `source/theme.*` (5 themes + `C()` const-color macro), `source/osk.*` (keyboard), `source/ui.*` (Mode-3 UI + `ui_set/get_repeat_mask` wrapper).
- `HARDWARE-SIGNOFF.md` — the **B1–B44** master checklist (the gate to "done"). `RELEASE-STATUS.md`, `README.md`, `HARDWARE-TEST-PHASE1.md`.
- **Shared layer (pulled via `../../` from the `gba-toolkit` foundation repo):** `lib/{flashcartio,fatfs,ezflashomega,everdrivegbax5}`, `source/{gba_rtc,log}`. The reboot is in `lib/flashcartio.c` + `lib/ezflashomega/io_ezfo.c` (foundation commit `31dab18`).
- **Foundation knowledge:** `gba-toolkit/docs/CAPABILITIES.md` (§1b media verdicts, §2.6 reboot), `docs/kb/gotchas-and-techniques.md` (committed `722341e`), `docs/ROADMAP.md` §A Phase 6.

## Gotchas / constraints

- **OS-mode:** ROM is unmapped during any EZ-Flash SD read → **read into EWRAM, *then* render** (VRAM stays mapped); never draw mid-transfer. Consequence: no decode-while-streaming (why audio/video streaming is impossible).
- **Big buffers in `EWRAM_BSS`, never the 32 KiB IWRAM stack**; recursion via the explicit dir-stack (depth ≤ `FS_RMTREE_MAX_DEPTH`=24).
- **`ui_truncate` caps display COLUMNS not bytes** → size on-screen-string buffers `4*cols + 1` (a UTF-8 codepoint is up to 4 bytes/col; this caused 3 stack overflows once).
- **`key_repeat()` once per frame**; modals save/restore the repeat mask via `ui_set/get_repeat_mask`.
- **Writes have no retry** → user-data edits go through the verified-write pattern; reserved temp namespaces `.sdbtmp~` / `.bak~` / `.hexnew~`.
- **Not "done" until hardware sign-off** — SD/RTC/reset are not emulated.

## Related docs

- `HARDWARE-SIGNOFF.md` (the sign-off checklist) · `RELEASE-STATUS.md` · `README.md` (controls + feature list).
- Foundation: `gba-toolkit/docs/ROADMAP.md` §A, `docs/CAPABILITIES.md`, `docs/kb/gotchas-and-techniques.md`.

---

## Session log

### Session — 2026-06-10

- **Intent:** Close out / checkpoint the SD file browser. User: *"what did you want me to test before we close this sd-browser project?"* + *"I would also like to create a github repository after that"* + *"use the handoff skill here."*
- **Did:** Across this chat — built **P4** (settings / 5 themes / `/sdbrowse.cfg` persistence / last-folder / show-hidden / reboot-to-loader), **P5** (START⇄SELECT swap, scrolling actions menu, folder-size, duplicate, new-file, swap-names, go-to-offset, reset-settings, recursive keyword find, keyboard auto-repeat), and **P6a–c** (open-by-extension dispatcher, word-wrap text viewer, BMP image viewer). Each batch went through an adversarial **find→verify** multi-agent review; committed per-phase (`61783e8`, `7b58a1a` here; foundation lib reboot `31dab18`, docs `4ef21ea`/`2bcdce8`/`722341e`). Ran a media-feasibility study → recorded the verdicts (mp3/mp4/pdf infeasible) + the Phase-6 plan in ROADMAP, and the cross-cutting techniques in `docs/kb/gotchas-and-techniques.md`. **Reboot-to-loader confirmed working on real Omega DE hardware.** Produced the prioritized hardware-test list (Tiers 1–4 over B1–B44).
- **Left off:** Feature-complete and fully committed on `main` (not pushed). Awaiting the user's **real-hardware sign-off** (B1–B44) and the **GitHub-repo** prep. No code mid-edit.
- **Open threads:** (1) HW test results to fold into `HARDWARE-SIGNOFF.md`. (2) GitHub repo = vendor `lib/` + pick a license + licensing review + push (gh not installed) — deferred to after testing. (3) Optional P6d/e/f viewers remain planned in ROADMAP §A Phase 6.
