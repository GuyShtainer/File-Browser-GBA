#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""vsd_shots.py -- the README screenshots, taken on a demo card under the virtual SD.

    python3 tools/vsd_shots.py [--out DIR]       (default: docs/screenshots)

Needs `make vsd`. Builds ONE 320 MiB demo card (tools/vsd_demo.py: a names-only mirror of a
real EZ-Flash Omega DE card, zero-filled sparse bodies) and drives one mGBA session through
the browser's key map, writing 13 PNGs at 1x (240x160). Every shot asserts something
mechanical first (cwd, pin count, list order) -- the PNG itself is only the picture.
After the run the card diff must show ONLY /file_browser_gba/* and /.sdtrash/* paths.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import vsd_demo  # noqa: E402
import vsd_diff  # noqa: E402
import vsd_run   # noqa: E402
from vsd_run import Runner, RunnerError  # noqa: E402

ROM = ROOT / "file_browser_gba-vsd.gba"
WORK = Path("/tmp/fb-vsd-shots")
CARD_MIB = 320
MIN_FREE_PCT = 15
SETTLE = 60          # frames after a key: a rescan/sort keeps the CPU busy after the last SD request
                     # (the screen shows a static "Reading..." meanwhile) and a key tapped then is lost
NAMES = ["root-pins", "saver", "grid", "columns", "start-menu", "pin-menu", "txtedit",
         "txtedit-save", "hex", "buttons", "settings", "trash", "many"]


class ShotFail(Exception):
    """A mechanical pre-shot assertion failed."""


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise ShotFail(msg)


def build_demo_card() -> Path:
    """Expands the manifest + overlay and mkimg's the 320 MiB card; asserts >= 15 % free."""
    tree = WORK / "demo.tree"
    shutil.rmtree(tree, ignore_errors=True)
    WORK.mkdir(parents=True, exist_ok=True)
    vsd_demo.expand(tree)
    img = WORK / "demo.img"
    subprocess.run([str(vsd_run.ensure_vsd_img()), "mkimg", str(img), str(CARD_MIB), str(tree),
                    "file_browser_gba"], check=True, capture_output=True)
    shutil.rmtree(tree, ignore_errors=True)
    used = sum(int(line.split()[-2]) for line in vsd_run.img_list(img).splitlines() if line.strip())
    free_pct = 100.0 * (1 - used / (CARD_MIB * 1048576))
    check(free_pct >= MIN_FREE_PCT, f"card only {free_pct:.1f}% free by file sizes")
    print(f"card: {CARD_MIB} MiB, files total {used / 1048576:.1f} MiB, >= {free_pct:.0f}% free")
    return img


class Session:
    """One Runner + the output folder."""

    def __init__(self, img: Path, out: Path):
        self.r = Runner(ROM, img)
        self.r.run(180)
        self.r.quiesce()
        self.out = out
        out.mkdir(parents=True, exist_ok=True)
        self.taken: "list[str]" = []

    def keys(self, *names: str) -> None:
        for name in names:
            count = 1
            if name.startswith("*"):
                count_text, name = name[1:].split(":")
                count = int(count_text)
            for _ in range(count):
                self.r.tap(name, settle=SETTLE)

    def shot(self, name: str) -> None:
        self.r.shot(self.out / f"{name}.png")
        self.taken.append(name)

    def cwd(self) -> str:
        return self.r.peek_cstr("g_cwd")

    def pins(self) -> int:
        return self.r.peek_u32("g_pins", 12)

    def first_pin(self) -> str:
        pool = self.r.peek_u32("g_pins", 0)
        out = bytearray()
        for i in range(255):
            byte = self.r.core.memory.u8.raw_read(pool + i) & 0xFF
            if byte == 0:
                break
            out.append(byte)
        return out.decode()


def view_mode(s: Session) -> int:
    return s.r.peek_u32("g_set", 36)          # Settings.view_mode at +36 after five bools (cfg.h: 0 list, 1 grid, 2 columns)


def open_settings(s: Session) -> None:
    """START menu with the two shortcut rows: UP x4 from the first action = Settings."""
    s.keys("START", "*4:UP", "A")


def take_all(s: Session) -> None:
    check(s.pins() == 3 and s.first_pin() == "/SAVER", f"pins {s.pins()} first {s.first_pin()!r}")
    check(s.r.peek_u32("g_shortcuts", 12) == 2, "demo shortcuts not loaded")
    s.keys("SELECT")                                  # cfg says Date new-old: one tap -> Name A-Z
    check(s.r.peek_u8("g_sortkey") == 0 and s.r.peek_u8("g_sortrev") == 0, "sort not Name A-Z")
    s.shot("root-pins")
    s.keys("*3:UP", "A")                              # three pin rows above: row 0 = /SAVER
    check(s.cwd() == "/SAVER", f"cwd {s.cwd()!r}")
    s.keys("DOWN")                                    # a file under the cursor shows its date
    s.shot("saver")
    s.keys("B")
    check(s.cwd() == "/", "B did not return to /")

    s.keys("START", "*2:UP", "A")                     # first shortcut row = /tools/PokeDNA.gba
    check(s.cwd() == "/tools", f"shortcut -> cwd {s.cwd()!r}")
    open_settings(s)
    s.keys("DOWN", "RIGHT", "DOWN", "RIGHT", "A")     # View: Grid x3, Sort: Name A-Z, save
    check(view_mode(s) == 1, f"view_mode {view_mode(s)}")
    check(s.r.peek_u8("g_sortkey") == 0 and s.r.peek_u8("g_sortrev") == 0, "sort not Name A-Z")
    s.shot("grid")

    open_settings(s)
    s.keys("DOWN", "LEFT", "LEFT", "A")               # Grid -> List -> Columns
    check(view_mode(s) == 2, f"view_mode {view_mode(s)}")
    s.keys("B", "DOWN", "A")                          # up to /, tools -> _nds, enter
    check(s.cwd() == "/_nds", f"cwd {s.cwd()!r}")
    s.keys("*4:DOWN", "A")                            # TWiLightMenu is the 4th folder
    check(s.cwd() == "/_nds/TWiLightMenu", f"cwd {s.cwd()!r}")
    s.keys("DOWN")                                    # a folder under the cursor fills the right pane
    s.shot("columns")

    open_settings(s)
    s.keys("DOWN", "RIGHT", "A")                      # Columns wraps back to List
    check(view_mode(s) == 0, f"view_mode {view_mode(s)}")
    s.keys("B", "B")
    check(s.cwd() == "/", f"cwd {s.cwd()!r}")
    s.keys("START")                                   # two shortcut rows above the first action
    s.shot("start-menu")
    s.keys("*7:DOWN")                                 # cursor onto 'Pin to top' (index 7 of a folder)
    s.shot("pin-menu")
    s.keys("B")

    s.keys("UP", "A", "DOWN", "DOWN", "A")            # /notes.txt pin row > actions > Edit text
    s.keys("A")                                       # editor: A opens the on-screen keyboard
    s.shot("txtedit")
    s.keys("*2:DOWN", "*5:RIGHT", "A")                # row 2 col 5 = 'h'
    s.keys("UP", "*2:RIGHT", "A")                     # row 1 col 7 = 'i'
    s.keys("START")                                   # close the keyboard: nav mode, buffer modified
    s.shot("txtedit-save")
    s.keys("B", "DOWN", "A")                          # B -> 'Unsaved changes' > Exit without saving                               # Exit without saving: the card stays as it was
    check(s.cwd() == "/" and s.pins() == 3, f"after the editor cwd {s.cwd()!r} pins {s.pins()}")

    s.keys("B", "DOWN", "START", "UP", "A")        # B closes the actions menu the editor was opened from                        # second shortcut row = /SAVER
    check(s.cwd() == "/SAVER", f"cwd {s.cwd()!r}")
    s.keys("*3:L", "*13:DOWN")                        # L jumps 11 rows: 3 presses clamp at the top; 13 DOWN = row 14/27
    s.keys("A", "DOWN", "A")              # POKEMON_EMER_BPEE00.sav > actions > View
    s.shot("hex")
    s.keys("B")                                       # back to the /SAVER listing

    open_settings(s)
    s.keys("*12:DOWN", "A")                           # Button shortcuts...
    s.keys("A", "*11:DOWN", "A")                      # SEL+UP -> Bind to... -> Go to root
    s.keys("DOWN", "A", "*4:DOWN", "A")               # SEL+DOWN -> Bind to... -> Trash
    s.shot("buttons")
    bound = sorted(s.r.cat("/file_browser_gba/buttons.txt").split(b"\n"))
    check(bound == [b"", b"DOWN=act:trash", b"UP=act:root"], f"buttons.txt {bound!r}")
    s.keys("B", "B")                                  # slots -> Settings -> browser (B = cancel)
    check(s.cwd() == "/SAVER", f"cwd {s.cwd()!r}")

    open_settings(s)
    s.shot("settings")
    s.keys("B")

    s.keys("B")                                       # up to /
    check(s.cwd() == "/", f"cwd {s.cwd()!r}")
    s.keys("*3:L", "*3:DOWN", "A")                    # top (3 pin rows), CHEAT is the first entry
    check(s.cwd() == "/CHEAT", f"cwd {s.cwd()!r}")
    s.keys("DOWN", "A")                               # Eng
    check(s.cwd() == "/CHEAT/Eng", f"cwd {s.cwd()!r}")
    s.keys("DOWN", "A")                               # 0000 (the 120-file folder)
    check(s.cwd() == "/CHEAT/Eng/0000", f"cwd {s.cwd()!r}")
    s.keys("*5:R")                                    # five row jumps: mid-scroll in a 124-row folder
    s.shot("many")
    s.keys("A", "*11:DOWN", "A", "A")                 # file actions > Move file to Trash > yes
    s.r.run(400)                                      # the rescan outlasts the settle; START would be lost
    s.keys("START", "*5:UP", "A")                     # START > Trash (recycle bin)...
    s.shot("trash")
    s.keys("B")
    trashed = [ln for ln in s.r.listing().splitlines() if ln.startswith("/.sdtrash/") and ".cht " in ln]
    check(len(trashed) == 1, f"expected one trashed .cht, found {trashed}")


def audit_card(s: Session) -> None:
    """The shots may only have touched the app folder and the trash."""
    s.r.flush()
    before = vsd_diff.parse_text(s.r._pre_list)          # the card as it was at attach
    after = vsd_diff.parse_text(vsd_run.img_list(s.r.img))
    added, removed, changed = vsd_diff.diff_lists(before, after)
    touched = sorted(set(added) | set(removed) | set(changed))
    trashed = {Path(p).name for p in added if p.startswith("/.sdtrash/")}
    stray = [p for p in touched if not p.startswith(("/file_browser_gba/", "/.sdtrash/"))
             and not (p in removed and Path(p).name in trashed)]   # the moved file left its folder
    check(not stray, f"the shots changed files outside the app folder/trash: {stray}")
    print(f"card diff: {len(touched)} paths: the app folder, the trash and the trashed file's old name")


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "screenshots")
    args = ap.parse_args(argv)
    if not ROM.is_file():
        print(f"missing {ROM}: run `make vsd` first", file=sys.stderr)
        return 2
    img = build_demo_card()
    sess = Session(img, args.out)
    try:
        take_all(sess)
        audit_card(sess)
    except (ShotFail, RunnerError) as exc:
        print(f"FAIL after {sess.taken}: {exc}", file=sys.stderr)
        return 1
    missing = [n for n in NAMES if n not in sess.taken]
    check_missing = f"missing shots: {missing}" if missing else ""
    if check_missing:
        print(check_missing, file=sys.stderr)
        return 1
    print(f"shots: {sess.taken}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
