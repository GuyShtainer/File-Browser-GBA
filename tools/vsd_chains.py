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


CHAINS = {1: chain_1, 2: chain_2, 3: chain_3, 4: chain_4, 5: chain_5, 6: chain_6, 7: chain_7}


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
