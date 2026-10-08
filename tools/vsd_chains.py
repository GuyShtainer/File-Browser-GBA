#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""vsd_chains.py -- File-Browser-GBA fault-injection chains on the virtual SD card.

    python3 tools/vsd_chains.py --all          # every chain, table + exit status
    python3 tools/vsd_chains.py --only 7       # one chain

Needs `make vsd` first (file_browser_gba-vsd.gba + .elf) and /tmp/vsd_img
(tools/build_vsd_host.sh builds it). Each chain boots a FRESH 32 MiB card built from
tools/vsd_template/; the verdict is the card diff, the card's bytes and the card's log --
screenshots (chains/<name>/*.png) are for human eyes, never the pass/fail oracle (the one
pixel comparison, the save-status strip, is labelled where it is used).
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import vsd_run  # noqa: E402
from vsd_run import Runner, RunnerError  # noqa: E402

ROM = ROOT / "file_browser_gba-vsd.gba"
SHIPPED_ELF = ROOT / "file_browser_gba.elf"
TEMPLATE = HERE / "vsd_template"
OUT = ROOT / "chains"
WORK = Path("/tmp/fb-vsd-chains")
APP = "file_browser_gba"
LOG = f"/{APP}/log.txt"
NOTES = b"alpha\nbeta\ngamma"
WIN = b"one\r\ntwo\r\nthree"
ROOT_IDX = dict(docs=0, file_browser_gba=1, roms=2, big=3, empty=4, cfg=5, mixed=6,
                notes=7, win=8)


class ChainFail(Exception):
    """An assertion about the card/log/UI failed."""


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise ChainFail(msg)


# -- card and flow helpers -----------------------------------------------------------
def build_card(name: str, plant: "dict | None" = None) -> Path:
    """Fresh 32 MiB card from the template (+ an empty roms/) at WORK/<name>.img.
    `plant` = {host-tree relative path: bytes} added to the tree before mkimg."""
    tree = WORK / f"{name}.tree"
    shutil.rmtree(tree, ignore_errors=True)
    shutil.copytree(TEMPLATE, tree)
    (tree / "roms").mkdir()
    for rel, data in (plant or {}).items():
        dest = tree / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
    img = WORK / f"{name}.img"
    subprocess.run([str(vsd_run.ensure_vsd_img()), "mkimg", str(img), "32", str(tree), APP],
                   check=True, capture_output=True)
    shutil.rmtree(tree, ignore_errors=True)
    return img


def boot(img: Path, **knobs) -> Runner:
    runner = Runner(ROM, img, **knobs)
    runner.run(180)
    return runner


def pick(r: Runner, index: int) -> None:
    """Cursor to root-list row `index` (from row 0)."""
    for _ in range(index):
        r.tap("DOWN")


def open_edit(r: Runner, index: int) -> None:
    """Root row `index` -> A (actions menu) -> Edit text (3rd row) -> A."""
    pick(r, index)
    r.tap("A")
    r.tap("DOWN")
    r.tap("DOWN")
    r.tap("A")


def kbd_type_h(r: Runner) -> None:
    """Keyboard open (cursor row0/col0): 'h' = row 2 col 5."""
    r.tap("DOWN"); r.tap("DOWN")
    for _ in range(5):
        r.tap("RIGHT")
    r.tap("A")


def kbd_type_enter(r: Runner) -> None:
    """Keyboard open: UP wraps to row 4 (Space), RIGHT -> Enter."""
    r.tap("UP")
    r.tap("RIGHT")
    r.tap("A")


def editor_keyboard_type(r: Runner, typer) -> None:
    """In the editor: A opens the keyboard, `typer` types, START closes it."""
    r.tap("A")
    typer(r)
    r.tap("START")


def editor_save(r: Runner) -> None:
    """Editor nav mode: START opens the menu, A on its first row (Save)."""
    r.tap("START")
    r.tap("A")


def log_text(r: Runner) -> str:
    return r.cat(LOG).decode("utf-8", "replace")


def status_strip(r: Runner) -> bytes:
    """Raw pixels of the editor status strip (y 148..160) -- the ONLY pixel oracle, used by
    the sweeps to learn what the product itself claims ('saved' vs an error screen)."""
    img = r.screen.to_pil().convert("RGB").crop((0, 148, 240, 160))
    return img.tobytes()


class Ctx:
    """Per-chain output folder + result lines."""

    def __init__(self, name: str):
        self.name = name
        self.dir = OUT / name
        shutil.rmtree(self.dir, ignore_errors=True)
        self.dir.mkdir(parents=True)
        self.lines: list = []

    def shot(self, r: Runner, label: str) -> None:
        r.shot(self.dir / f"{label}.png")

    def note(self, text: str) -> None:
        self.lines.append(text)


# -- chains --------------------------------------------------------------------------
def chain_1(c: Ctx) -> None:
    """boot+list: root listing, nothing but the boot log changed."""
    r = boot(build_card("c1"))
    c.shot(r, "root")
    got = r.report([], optional={LOG})
    c.note(f"changed={sorted(got)}")
    check("virtual SD" in log_text(r), "boot log lacks the virtual SD line")
    check(r.peek_cstr("g_cwd") == "/", f"cwd {r.peek_cstr('g_cwd')!r}")


def _edit_chain(c: Ctx, idx: int, typer, orig: bytes, new: bytes, name: str, path: str) -> None:
    r = boot(build_card(c.name))
    open_edit(r, idx)
    c.shot(r, "editor")
    editor_keyboard_type(r, typer)
    c.shot(r, "typed")
    editor_save(r)
    c.shot(r, "saved")
    r.report({path, path + ".bak~"}, optional={LOG, f"/{APP}/"})
    got = r.cat(path)
    check(got == new, f"{path}: {got!r} != {new!r}")
    check(r.cat(path + ".bak~") == orig, f"{path}.bak~ is not the original")
    check(f"txtedit: saved {path}" in log_text(r), "log lacks the saved line")
    c.note(f"{path} {len(orig)}->{len(got)} B; bak==orig; temp gone")
    check(b".txtnew~" not in r.listing().encode(), "temp left behind")


def chain_2(c: Ctx) -> None:
    """edit LF: insert 'hi' at offset 0 of notes.txt and save."""
    def typer(r: Runner) -> None:
        kbd_type_h(r)
        r.tap("UP"); r.tap("RIGHT"); r.tap("RIGHT")   # row 1 col 7 = 'i'
        r.tap("A")
    _edit_chain(c, ROOT_IDX["notes"], typer, NOTES, b"hi" + NOTES, "notes", "/notes.txt")


def chain_3(c: Ctx) -> None:
    """edit CRLF: Enter typed once at offset 0 gives \\r\\n, never a lone \\n or \\r."""
    _edit_chain(c, ROOT_IDX["win"], kbd_type_enter, WIN, b"\r\n" + WIN, "win", "/win.txt")


def chain_4(c: Ctx) -> None:
    """binary kept: mixed.bin edit after the warning keeps every original byte."""
    img = build_card("c4")
    orig = vsd_run.img_cat(img, "/mixed.bin")
    check(len(orig) == 200 and b"\x00" in orig and b"\xff" in orig, "template mixed.bin shape")
    r = boot(img)
    open_edit(r, ROOT_IDX["mixed"])
    c.shot(r, "warning")
    r.tap("A")                                   # Confirm: Yes (default row)
    c.shot(r, "editor")
    editor_keyboard_type(r, kbd_type_h)
    editor_save(r)
    c.shot(r, "saved")
    r.report({"/mixed.bin", "/mixed.bin.bak~"}, optional={LOG, f"/{APP}/"})
    got = r.cat("/mixed.bin")
    check(got == b"h" + orig, f"mixed.bin {len(got)} B: bytes after the insert differ")
    check(r.cat("/mixed.bin.bak~") == orig, "bak is not the original")
    c.note(f"200 -> {len(got)} B, all 200 original bytes intact after the inserted 'h'")


def _menu_row(r: Runner, row: int) -> bytes:
    """Pixels of actions-menu row `row` (12 px pitch from y=16); row 0 is the selected one."""
    top = 16 + 12 * row
    return r.screen.to_pil().convert("RGB").crop((8, top, 230, top + 12)).tobytes()


def chain_5(c: Ctx) -> None:
    """cap: big.txt (> TB_CAP) actions menu has no Edit text row."""
    r = boot(build_card("c5"))
    pick(r, ROOT_IDX["notes"])
    r.tap("A")
    c.shot(r, "notes-menu")
    notes_edit, notes_find = _menu_row(r, 2), _menu_row(r, 3)
    check(notes_edit != notes_find, "crop rows are not distinct (geometry wrong)")
    r.tap("B")
    r.tap("UP")                                   # notes -> mixed ... back up to big.txt
    for _ in range(ROOT_IDX["notes"] - 1 - ROOT_IDX["big"]):
        r.tap("UP")
    r.tap("A")
    c.shot(r, "big-menu")
    check(_menu_row(r, 2) != notes_edit, "big.txt menu row 2 is 'Edit text'")
    check(_menu_row(r, 2) == notes_find, "big.txt menu row 2 is not 'Find...'")
    r.report([], optional={LOG})
    c.note("big.txt (40000 B) menu row 2 == notes.txt menu 'Find...' row, != its 'Edit text' row")


def chain_6(c: Ctx) -> None:
    """leftover temp: a stray notes.txt.txtnew~ makes the save refuse; notes.txt untouched."""
    img = build_card("c6", plant={"notes.txt.txtnew~": b"STRAY"})
    r = boot(img)
    open_edit(r, ROOT_IDX["notes"])
    editor_keyboard_type(r, kbd_type_h)
    editor_save(r)
    c.shot(r, "refused")
    r.report([], optional={LOG})
    check(r.cat("/notes.txt") == NOTES, "notes.txt changed")
    check(r.cat("/notes.txt.txtnew~") == b"STRAY", "stray temp was touched")
    check("txtedit: refused leftover temp /notes.txt" in log_text(r), "log lacks the refusal")
    c.note("refused leftover temp; card changed only by the log")


def _edit_run(tag: str, pristine: Path, **knobs) -> dict:
    """Chain 2's flow on a copy of the pristine card under `knobs`; returns the observations."""
    img = WORK / f"sweep-{tag}.img"
    shutil.copyfile(pristine, img)
    r = boot(img, **knobs)
    open_edit(r, ROOT_IDX["notes"])
    editor_keyboard_type(r, _type_hi)
    editor_save(r)
    strip = status_strip(r)
    r.flush()
    try:
        notes = r.cat("/notes.txt")
    except RunnerError:
        notes = None
    try:
        temp = r.cat("/notes.txt.txtnew~")
    except RunnerError:
        temp = None
    try:
        bak = r.cat("/notes.txt.bak~")
    except RunnerError:
        bak = None
    try:
        log = log_text(r)
    except RunnerError:
        log = ""
    listing = r.listing()
    counters = r.counters
    pre_list = r._pre_list
    return dict(notes=notes, bak=bak, temp=temp, log=log, strip=strip, counters=counters,
                temp_left=".txtnew~" in listing, listing=listing, pre=pre_list)


def _type_hi(r: Runner) -> None:
    kbd_type_h(r)
    r.tap("UP"); r.tap("RIGHT"); r.tap("RIGHT")
    r.tap("A")


NEW_NOTES = b"hi" + NOTES


def _classify(obs: dict, ref_strip: bytes) -> "tuple[str, str, str, str]":
    """(notes state, bak state, log verdict, ui verdict) -- ui = the status strip pixels vs a
    knob-free run (the product's own claim of 'saved'). Raises ChainFail on an invariant break."""
    recover = obs["notes"] is None and obs["bak"] == NOTES and obs["temp"] == NEW_NOTES
    notes = ("RECOVER" if recover else "MISSING" if obs["notes"] is None else "ORIG" if obs["notes"] == NOTES else
             "NEW" if obs["notes"] == NEW_NOTES else "CORRUPT")
    bak = "-" if obs["bak"] is None else "orig" if obs["bak"] == NOTES else "BAD"
    log = ("saved" if "txtedit: saved" in obs["log"] else
           "failed" if ("save failed" in obs["log"] or "refused" in obs["log"]) else "none")
    ui = "believes-saved" if obs["strip"] == ref_strip else "shows-other"
    return notes, bak, log, ui


def _sweep(c: Ctx, knob: str, pristine: Path, ref: dict, total: int) -> list:
    rows, problems = [], []
    c.note(f"{knob} sweep, k=0..{total} (W={total} write sectors in the knob-free run)")
    c.note("  k | notes   | bak  | log    | ui")
    for k in range(total + 1):
        obs = _edit_run(f"{knob}{k}", pristine, **{knob: k})
        notes, bak, log, ui = _classify(obs, ref["strip"])
        c.note(f" {k:2d} | {notes:<7} | {bak:<4} | {log:<6} | {ui}")
        rows.append((k, notes, bak, log, ui))
        if notes in ("CORRUPT", "MISSING"):
            problems.append(f"{knob}={k}: notes.txt is {notes} (neither original nor new)")
        if bak == "BAD":
            problems.append(f"{knob}={k}: .bak~ present but not the original")
        if notes == "RECOVER" and ui == "believes-saved":
            problems.append(f"{knob}={k}: notes.txt MISSING on the card but the UI says saved")
        elif notes == "RECOVER" and knob == "fail_at" and log != "failed":
            problems.append(f"{knob}={k}: RECOVER state but the log does not say save failed")
        if ui == "believes-saved" and notes != "NEW":
            problems.append(f"{knob}={k}: UI says saved but notes.txt is {notes}")
        if log == "saved" and notes != "NEW":
            problems.append(f"{knob}={k}: log says saved but notes.txt is {notes}")
        if knob == "fail_at" and obs["temp_left"] and notes in ("NEW", "ORIG"):
            problems.append(f"{knob}={k}: temp left behind after a completed save")
    check(not problems, "; ".join(problems))
    return rows


def chain_7(c: Ctx) -> None:
    """sweeps: fail_at / lie_after over every write sector of the notes.txt save."""
    pristine = build_card("c7pristine")
    ref = _edit_run("ref", pristine)
    total = ref["counters"]["writes_served"]
    check(ref["notes"] == NEW_NOTES, "knob-free reference run did not save")
    c.note(f"reference: writes_served={total}, reads={ref['counters']['reads_served']}")
    _sweep(c, "fail_at", pristine, ref, total)
    _sweep(c, "lie_after", pristine, ref, total)
    prot = _edit_run("protect", pristine, protect=True)
    notes, bak, log, ui = _classify(prot, ref["strip"])
    c.note(f"protect=True: notes={notes} bak={bak} log={log} ui={ui}")
    check(notes == "ORIG" and bak == "-", "protect: card changed")
    check(ui != "believes-saved", "protect: UI claims saved on a write-protected card")


PINS = f"/{APP}/pins.txt"
SHORTCUTS = f"/{APP}/shortcuts.txt"
BUTTONS = f"/{APP}/buttons.txt"
SETTINGS = f"/{APP}/settings.cfg"


def reboot(r: Runner, **knobs) -> Runner:
    """Power-cycle: flush the card, boot a NEW Runner (fresh snapshot) on the same image."""
    r.flush()
    return boot(r.img, **knobs)


def pin_count(r: Runner) -> int:
    return r.peek_u32("g_pins", 12)


def chain_8(c: Ctx) -> None:
    """pins: pin, reboot, open, unpin, pin+trash target, 'Not found. Unpin?'."""
    r = boot(build_card("c8"))
    r.tap("START")
    for _ in range(7):
        r.tap("DOWN")
    r.tap("A")                                      # Pin to top on /docs
    c.shot(r, "pinned")
    r.report({PINS}, optional={LOG})
    check(r.cat(PINS) == b"/docs\n", f"pins.txt {r.cat(PINS)!r}")
    check("pins: saved 1" in log_text(r), "log lacks 'pins: saved 1'")
    check(pin_count(r) == 1, "pin count in RAM")

    r = reboot(r)                                   # pins survive a reboot
    check(pin_count(r) == 1, f"pin count after reboot {pin_count(r)}")
    c.shot(r, "reboot-first-row")
    r.tap("UP")                                     # cursor starts on the first real row
    r.tap("A")
    check(r.peek_cstr("g_cwd") == "/docs", f"pin row A -> cwd {r.peek_cstr('g_cwd')!r}")
    c.shot(r, "pin-opened")

    r.tap("B")                                      # back to /, then the pin menu
    r.tap("UP")
    r.tap("START")
    c.shot(r, "pin-menu")
    r.tap("DOWN")
    r.tap("A")                                      # Unpin
    check(pin_count(r) == 0, "unpin did not drop the pin")
    r.report({PINS}, optional={LOG})
    check(r.cat(PINS) == b"", f"pins.txt after unpin {r.cat(PINS)!r}")

    r = reboot(r)
    r.tap("A")                                      # enter /docs (cursor on docs)
    r.tap("DOWN")                                   # sub/
    r.tap("START")
    for _ in range(7):
        r.tap("DOWN")
    r.tap("A")                                      # Pin to top on /docs/sub
    check(r.cat(PINS) == b"/docs/sub\n", f"pins.txt {r.cat(PINS)!r}")
    r.tap("START")
    for _ in range(10):
        r.tap("DOWN")
    r.tap("A")                                      # Move folder to Trash
    c.shot(r, "trash-confirm")
    r.tap("A")                                      # confirm
    lst = r.listing()
    check("/.sdtrash/sub/b.txt" in lst and "/docs/sub/" not in lst, "sub was not trashed")

    r = reboot(r)
    r.tap("UP")
    r.tap("A")                                      # pin row -> target is gone
    c.shot(r, "not-found")
    check(r.peek_cstr("g_cwd") == "/" and pin_count(r) == 1, "dialog left the pin/cwd changed")
    r.tap("A")                                      # Unpin it
    check(pin_count(r) == 0, "'Unpin it?' yes did not unpin")
    r.report({PINS}, optional={LOG})
    check(r.cat(PINS) == b"", "pins.txt not empty after the dangling unpin")
    c.note("pin/open/unpin, trashed target -> 'Not found. Unpin?' -> unpinned; pins.txt exact each step")


def chain_9(c: Ctx) -> None:
    """shortcuts: add on /roms, reboot, START-menu row opens it, SELECT removes it."""
    r = boot(build_card("c9"))
    pick(r, ROOT_IDX["roms"])
    r.tap("START")
    for _ in range(11):
        r.tap("DOWN")
    r.tap("A")                                      # Add shortcut
    r.report({SHORTCUTS}, optional={LOG})
    check(r.cat(SHORTCUTS) == b"/roms\n", f"shortcuts.txt {r.cat(SHORTCUTS)!r}")
    check("shortcuts: saved 1" in log_text(r), "log lacks 'shortcuts: saved 1'")

    r = reboot(r)
    r.tap("START")
    c.shot(r, "start-menu")
    r.tap("UP")                                     # the shortcut row sits above the first action
    r.tap("A")
    check(r.peek_cstr("g_cwd") == "/roms", f"shortcut -> cwd {r.peek_cstr('g_cwd')!r}")
    c.shot(r, "in-roms")

    r = reboot(r)
    r.tap("START")
    r.tap("UP")
    r.tap("SELECT")                                 # SELECT on a shortcut row removes it
    c.shot(r, "removed")
    r.report({SHORTCUTS}, optional={LOG})
    check(r.cat(SHORTCUTS) == b"", f"shortcuts.txt after remove {r.cat(SHORTCUTS)!r}")
    c.note("shortcut added/opened/removed; shortcuts.txt exact at each step")


def sort_state(r: Runner) -> int:
    """The live sort order as key*2 + reversed (g_sortkey / g_sortrev, single bytes)."""
    return r.peek_u8("g_sortkey") * 2 + (1 if r.peek_u8("g_sortrev") else 0)


def chain_10(c: Ctx) -> None:
    """buttons: bind SELECT+A to /docs and SELECT+UP to Go to root; chords run after a reboot."""
    r = boot(build_card("c10"))
    r.tap("START")
    for _ in range(12):
        r.tap("DOWN")
    r.tap("A")                                      # docs > Bind to button...
    for _ in range(4):
        r.tap("DOWN")
    c.shot(r, "slot-picker")
    r.tap("A")                                      # SEL+A
    check(r.cat(BUTTONS) == b"A=path:/docs\n", f"buttons.txt {r.cat(BUTTONS)!r}")
    c.shot(r, "after-bind")
    r.tap("B")                                      # the actions menu stays open after a bind

    r.tap("START")                                  # Settings is the second-to-last row
    r.tap("UP")
    r.tap("UP")
    r.tap("A")
    for _ in range(12):
        r.tap("DOWN")
    r.tap("A")                                      # Button shortcuts...
    c.shot(r, "slots")
    r.tap("A")                                      # SEL+UP -> Bind to...
    for _ in range(11):
        r.tap("DOWN")                               # None, This folder, then 9 actions -> Go to root
    r.tap("A")
    c.shot(r, "slots-bound")
    got = r.cat(BUTTONS)
    check(sorted(got.split(b"\n")) == sorted([b"A=path:/docs", b"UP=act:root", b""]),
          f"buttons.txt {got!r}")
    check("buttons: saved" in log_text(r), "log lacks 'buttons: saved'")

    r = reboot(r)
    sort0 = sort_state(r)
    check(r.peek_cstr("g_cwd") == "/", "boot cwd")
    r.tap("SELECT+A")
    check(r.peek_cstr("g_cwd") == "/docs", f"SELECT+A -> cwd {r.peek_cstr('g_cwd')!r}")
    c.shot(r, "chord-docs")
    r.tap("SELECT+UP")
    check(r.peek_cstr("g_cwd") == "/", f"SELECT+UP -> cwd {r.peek_cstr('g_cwd')!r}")
    check(sort_state(r) == sort0, "a chord changed the sort order")
    r.tap("SELECT")                                 # a plain tap still cycles the sort
    sort1 = sort_state(r)
    c.shot(r, "sort-cycled")
    check(sort1 != sort0, f"plain SELECT tap did not cycle sort ({sort0} -> {sort1})")
    c.note(f"buttons.txt={got!r}; chords navigate; plain SELECT sort {sort0}->{sort1}")


def chain_11(c: Ctx) -> None:
    """cfg migration: old root cfg read at boot, settings.cfg written under the app folder."""
    r = boot(build_card("c11"))
    check(r.peek_u32("g_set", 0) == 3, f"boot theme {r.peek_u32('g_set', 0)} (old root cfg says 3)")
    check(SETTINGS not in r.listing(), "settings.cfg exists before any save")
    c.shot(r, "boot-theme3")
    base_sort = sort_state(r)
    r.tap("START")
    r.tap("UP")
    r.tap("UP")
    r.tap("A")                                      # Settings
    r.tap("DOWN")
    r.tap("DOWN")                                   # Sort row
    r.tap("RIGHT")
    c.shot(r, "settings-sort-changed")
    r.tap("A")                                      # save + close
    r.report({SETTINGS}, optional={LOG})            # the old root cfg is NOT in the diff
    cfg = r.cat(SETTINGS).decode()
    check("theme=3" in cfg, f"migrated theme lost: {cfg!r}")
    new_sort = sort_state(r)
    check(new_sort != base_sort, "Sort row did not change the sort")
    want = f"sort_key={new_sort // 2}\nsort_rev={new_sort % 2}\n"
    check(want in cfg, f"settings.cfg lacks {want!r}: {cfg!r}")

    old = WORK / "c11-old.cfg"                      # make the OLD root file disagree, then reboot
    old.write_text("[file_browser_gba]\ntheme=1\nsort_key=2\nsort_rev=1\n")
    subprocess.run([str(vsd_run.ensure_vsd_img()), "patch", str(r.img), "/file_browser_gba.cfg",
                    str(old)], check=True, capture_output=True)
    r = boot(r.img)
    check(r.peek_u32("g_set", 0) == 3, "reboot took the OLD root cfg's theme")
    check(sort_state(r) == new_sort, f"reboot sort {sort_state(r)} != saved {new_sort}")
    c.shot(r, "reboot-new-cfg-wins")
    c.note(f"theme 3 migrated; sort {base_sort}->{new_sort} saved to settings.cfg; root cfg ignored after")


BUILD_ENV = {
    "DEVKITPRO": "/opt/devkitpro", "DEVKITARM": "/opt/devkitpro/devkitARM",
    "PATH": "/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:/usr/bin:/bin:/usr/sbin:/sbin",
    "HOME": str(Path.home()),
}
SEAM_FILES = ("lib/flashcartio.c", "lib/flashcartio_write.c")


def _build_tree(dest: Path) -> subprocess.CompletedProcess:
    return subprocess.run(["make", "-j4"], cwd=dest, env=BUILD_ENV, capture_output=True, text=True)


def chain_12(c: Ctx) -> None:
    """shipped-build guard: the shipped ELF has no vsd_ symbol and the VSD seam costs it zero bytes."""
    nm = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"
    out = subprocess.run([nm, str(SHIPPED_ELF)], capture_output=True, text=True, check=True).stdout
    hits = [ln for ln in out.splitlines() if "vsd_" in ln or "g_vsd" in ln]
    check(not hits, f"shipped ELF has VSD symbols: {hits[:3]}")
    c.note("nm shipped.elf | grep -c vsd_ == 0")

    guard = OUT / "guard"
    shutil.rmtree(guard, ignore_errors=True)
    lane = guard / "lane-novsd"
    lane.mkdir(parents=True)
    tar = subprocess.run(f"git -C {ROOT} ls-files -z | xargs -0 -I{{}} echo {{}} >/dev/null; "
                         f"git -C {ROOT} archive HEAD | tar -x -C {lane}", shell=True,
                         capture_output=True, text=True)
    check(tar.returncode == 0, f"git archive HEAD failed: {tar.stderr}")
    for rel in SEAM_FILES:                         # revert the seam to main's version
        main_src = subprocess.run(["git", "-C", str(ROOT), "show", f"main:{rel}"],
                                  capture_output=True, check=True).stdout
        (lane / rel).write_bytes(main_src)
    (lane / "lib/vsd.c").unlink()
    (lane / "lib/vsd.h").unlink()
    built = _build_tree(lane)
    check(built.returncode == 0, f"lane-without-seam build failed:\n{built.stderr[-600:]}")
    size = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-size"
    objs = sorted(p.name for p in (ROOT / "build").glob("*.o"))
    check("vsd.o" in objs, "lane build has no vsd.o (expected an empty translation unit)")
    differ = [n for n in objs if n != "vsd.o"
              and (ROOT / "build" / n).read_bytes() != (lane / "build" / n).read_bytes()]
    check(not differ, f"objects differ from the seam-reverted build: {differ}")
    vsd_o = subprocess.run([size, str(ROOT / "build/vsd.o")], capture_output=True, text=True).stdout
    check(vsd_o.split("\n")[1].split()[:3] == ["0", "0", "0"], f"vsd.o is not empty: {vsd_o!r}")
    sizes = [subprocess.run([size, "-A", str(e)], capture_output=True, text=True).stdout.split("\n", 1)[1]
             for e in (SHIPPED_ELF, lane / "file_browser_gba.elf")]
    check(sizes[0] == sizes[1], "section sizes differ from the seam-reverted build")
    a = (lane / "file_browser_gba.gba").read_bytes()
    b = (ROOT / "file_browser_gba.gba").read_bytes()
    check(len(a) == len(b), "image sizes differ")
    ndiff = sum(1 for x, y in zip(a, b) if x != y)
    c.note(f"{len(objs) - 1} objects byte-identical to the seam-reverted build; vsd.o = 0/0/0; "
           f"section sizes identical; final .gba differs in {ndiff} B (linker veneer order only: "
           f"same size, the empty vsd.o perturbs the link's symbol hash)")

    mainb = guard / "main"
    mainb.mkdir()
    tar = subprocess.run(f"git -C {ROOT} archive main | tar -x -C {mainb}", shell=True,
                         capture_output=True, text=True)
    check(tar.returncode == 0, "git archive main failed")
    built = _build_tree(mainb)
    check(built.returncode == 0, f"main build failed:\n{built.stderr[-600:]}")
    for label, elf in (("main", mainb / "file_browser_gba.elf"), ("lane", SHIPPED_ELF)):
        sec = subprocess.run([size, "-A", str(elf)], capture_output=True, text=True).stdout
        c.note(label + " sections: " + " ".join(
            f"{ln.split()[0]}={ln.split()[1]}" for ln in sec.splitlines()
            if ln.split() and ln.split()[0] in (".text", ".rodata", ".iwram", ".bss", ".data", ".ewram")))
    shutil.rmtree(guard, ignore_errors=True)


CHAINS = {1: chain_1, 2: chain_2, 3: chain_3, 4: chain_4, 5: chain_5, 6: chain_6, 7: chain_7, 8: chain_8, 9: chain_9, 10: chain_10, 11: chain_11, 12: chain_12}


def run_chain(num: int) -> "tuple[bool, str]":
    func = CHAINS[num]
    ctx = Ctx(f"{num:02d}-{func.__name__}")
    try:
        func(ctx)
        verdict, detail = True, "; ".join(ctx.lines)
    except (ChainFail, RunnerError, AssertionError) as exc:
        verdict = False
        detail = "\n".join(ctx.lines + [f"{type(exc).__name__}: {exc}"])
    except Exception:  # noqa: BLE001 -- a harness crash is a failure with its traceback
        verdict, detail = False, traceback.format_exc()
    (ctx.dir / "result.txt").write_text(("PASS" if verdict else "FAIL") + "\n" + detail + "\n")
    return verdict, detail


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    grp = ap.add_mutually_exclusive_group(required=True)
    grp.add_argument("--all", action="store_true")
    grp.add_argument("--only", type=int)
    args = ap.parse_args(argv)
    if not ROM.is_file():
        print(f"missing {ROM}: run `make vsd` first", file=sys.stderr)
        return 2
    WORK.mkdir(parents=True, exist_ok=True)
    nums = sorted(CHAINS) if args.all else [args.only]
    failed = 0
    for num in nums:
        ok, detail = run_chain(num)
        failed += 0 if ok else 1
        print(f"chain {num:2d} {CHAINS[num].__doc__.splitlines()[0][:60]:<60} "
              f"{'PASS' if ok else 'FAIL'}")
        print("   " + detail.replace("\n", "\n   "))
    print(f"{len(nums) - failed}/{len(nums)} chains passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
