#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""vsd_run.py -- generic runner for the virtual-SD harness (tools/vsd.py).

Boots a ROM built with -DVSD_ENABLE in mGBA (python bindings), serves its mailbox from a
FAT16 image (tools/vsd.py), injects keys, takes screenshots, and diffs the card before
and after. Importable (`Runner`) and a small CLI.

    from vsd_run import Runner
    r = Runner("tool-vsd.gba", "card.img", magic=b"GBTKVSD1", fail_at=3)
    r.run(180)                       # settle: frames advance ONLY through run()/tap()
    r.tap("START", "A")              # one key-down edge (or chord), then quiesce
    r.shot("out.png")
    r.report({"/notes.txt"})         # flush + diff vs the snapshot taken at attach
    r.peek_cstr("g_cwd")             # read a C string out of emulated memory (needs the .elf)
    print(r.cat("/notes.txt"), r.counters)

CLI:
    vsd_run.py ROM IMG --frames N [--keys "START,A,DOWN+A"] [--shot out.png]
               [--protect] [--fail-at N] [--lie-after N] [--fail-write-in N] ...

Frames: one SD transaction costs one emulated frame (the GBA spins until the host acks
after the frame ends), so a tap is followed by a QUIESCE: keep running until
QUIESCE_IDLE consecutive frames served no request, capped at QUIESCE_CAP (a cap hit is a
loud failure, never a silent pass).
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vsd        # noqa: E402
import vsd_diff   # noqa: E402

KEY = dict(A=0x1, B=0x2, SEL=0x4, SELECT=0x4, START=0x8, RIGHT=0x10, LEFT=0x20,
           UP=0x40, DOWN=0x80, R=0x100, L=0x200)
HOLD = 3             # frames a key is physically down
SETTLE = 12          # frames of nothing after a key edge
QUIESCE_IDLE = 4     # consecutive no-request frames that mean "I/O settled"
QUIESCE_CAP = 3600   # a tap that never quiesces is a loud failure
VSD_IMG_BIN = Path("/tmp/vsd_img")   # built by tools/build_vsd_host.sh
NM_FALLBACK = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"


class RunnerError(Exception):
    """A harness-level failure (missing tool, unexpected card change, no quiesce)."""


def _default_vendor() -> Path:
    """mGBA Python bindings: $VSD_MGBA_VENDOR, else rec2mp4's vendor dir next to this repo
    (../rec2mp4/vendor, i.e. rec2mp4 checked out next to this repo)."""
    here = Path(__file__).resolve()
    cands = [Path(os.environ["VSD_MGBA_VENDOR"])] if os.environ.get("VSD_MGBA_VENDOR") else []
    cands += [here.parents[1] / "projects" / "rec2mp4" / "vendor", here.parents[2] / "rec2mp4" / "vendor"]
    for c in cands:
        if c.is_dir():
            return c
    raise RunnerError("mGBA bindings not found; set VSD_MGBA_VENDOR to rec2mp4's vendor directory")


def load_mgba(vendor: "Path | None" = None):
    """Imports the mGBA bindings from `vendor` (default: rec2mp4's vendor dir, or the
    VSD_MGBA_VENDOR environment variable) and silences their per-instruction logging."""
    vend = Path(vendor) if vendor else _default_vendor()
    if not vend.is_dir():
        raise RunnerError(f"mGBA vendor path missing: {vend}")
    sys.path.insert(0, str(vend))
    import mgba.core, mgba.image, mgba.log   # noqa: E402,F401
    mgba.log.silence()                        # MANDATORY: else stderr floods
    return mgba.core, mgba.image


def ensure_vsd_img() -> Path:
    """Returns the host vsd_img binary, building it via tools/build_vsd_host.sh if absent."""
    if VSD_IMG_BIN.is_file():
        return VSD_IMG_BIN
    script = HERE / "build_vsd_host.sh"
    res = subprocess.run(["sh", str(script)], capture_output=True, text=True)
    if res.returncode != 0 or not VSD_IMG_BIN.is_file():
        raise RunnerError(f"build_vsd_host.sh failed:\n{res.stdout}\n{res.stderr}")
    return VSD_IMG_BIN


def img_list(img: Path, vsd_img: "Path | None" = None) -> str:
    """`vsd_img list IMG` output (sorted 'path size crc32' per file)."""
    res = subprocess.run([str(vsd_img or ensure_vsd_img()), "list", str(img)],
                         capture_output=True, text=True)
    if res.returncode != 0:
        raise RunnerError(f"vsd_img list {img} failed: {res.stderr.strip()}")
    return res.stdout


def img_cat(img: Path, path: str, vsd_img: "Path | None" = None) -> bytes:
    """The exact bytes of `path` inside `img` (raises RunnerError if absent)."""
    res = subprocess.run([str(vsd_img or ensure_vsd_img()), "cat", str(img), path],
                         capture_output=True)
    if res.returncode != 0:
        raise RunnerError(f"vsd_img cat {path} failed: {res.stderr.decode(errors='replace').strip()}")
    return res.stdout


def read_symbols(elf: Path) -> dict:
    """{name: address} for every data/bss symbol of `elf` (arm-none-eabi-nm; a name that
    occurs twice, e.g. two file-local statics, maps to None so a peek refuses to guess)."""
    nm = shutil.which("arm-none-eabi-nm") or NM_FALLBACK
    res = subprocess.run([nm, str(elf)], capture_output=True, text=True)
    if res.returncode != 0:
        raise RunnerError(f"nm {elf} failed: {res.stderr.strip()}")
    table: dict = {}
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] not in "bBdDrR":
            continue
        addr, name = int(parts[0], 16), parts[2]
        table[name] = None if name in table else addr
    return table


class Runner:
    """One booted core + its virtual card. Frames advance ONLY through run()/tap(), each
    followed by the server's service() -- never call core.run_frame() directly."""

    def __init__(self, rom: Path, img: Path, *, magic: bytes = vsd.VSD_LOCATOR_MAGIC,
                 vendor: "Path | None" = None, elf: "Path | None" = None, **knobs):
        core_mod, image_mod = load_mgba(vendor)
        self.rom = Path(rom)
        self.img = Path(img)
        self.core = core_mod.load_path(str(self.rom))
        if self.core is None:
            raise RunnerError(f"mGBA could not load {self.rom}")
        self.screen = image_mod.Image(*self.core.desired_video_dimensions())
        self.core.set_video_buffer(self.screen)   # BEFORE reset()
        self.core.reset()
        self.server = vsd.attach_server(self.core, self.rom.read_bytes(), self.img,
                                        magic=magic, **knobs)
        self.frames = 0
        self._elf = Path(elf) if elf else self.rom.with_suffix(".elf")
        self._symbols: "dict | None" = None
        self.snapshot()

    # -- frames and input ---------------------------------------------------------
    def run(self, n: int) -> int:
        """Runs `n` frames, serving the mailbox after each. Returns requests served."""
        served = 0
        for _ in range(n):
            self.core.run_frame()
            self.frames += 1
            if self.server.service():
                served += 1
        return served

    def quiesce(self) -> int:
        """Runs until QUIESCE_IDLE consecutive frames served nothing. Returns frames run."""
        idle = 0
        ran = 0
        while idle < QUIESCE_IDLE:
            if ran >= QUIESCE_CAP:
                raise RunnerError(f"I/O never quiesced after {ran} frames")
            idle = 0 if self.run(1) else idle + 1
            ran += 1
        return ran

    def tap(self, *names: str, hold: int = HOLD, settle: int = SETTLE) -> None:
        """One key-down edge. Several names are ONE simultaneous chord (SEL+UP), not a
        sequence -- use repeated tap() calls for a sequence."""
        mask = 0
        for name in names:
            for part in name.split("+"):
                if part not in KEY:
                    raise RunnerError(f"unknown key {part!r}")
                mask |= KEY[part]
        self.core.set_keys(raw=mask)
        self.run(hold)
        self.core.set_keys(raw=0)
        self.run(settle)
        self.quiesce()

    def shot(self, path: "Path | str") -> Path:
        """Saves the current frame as a PNG. Raises on a flat single-colour frame."""
        img = self.screen.to_pil().convert("RGB")
        lo, hi = img.convert("L").getextrema()
        if lo == hi:
            raise RunnerError(f"{path}: captured frame is one flat colour ({lo})")
        out = Path(path)
        out.parent.mkdir(parents=True, exist_ok=True)
        img.save(out)
        return out

    # -- peeking at emulated memory (needs the ROM's .elf next to it, or elf=) ----------
    def symbol(self, name: str) -> int:
        """Address of a data/bss symbol of the ROM's ELF (RunnerError if absent/ambiguous)."""
        if self._symbols is None:
            self._symbols = read_symbols(self._elf)
        addr = self._symbols.get(name)
        if addr is None:
            raise RunnerError(f"symbol {name!r} missing or ambiguous in {self._elf}")
        return addr

    def peek_u32(self, name: str, offset: int = 0) -> int:
        """The little-endian u32 at symbol `name` + `offset` (4-aligned)."""
        return self.core.memory.u32.raw_read(self.symbol(name) + offset) & 0xFFFFFFFF

    def peek_u8(self, name: str, offset: int = 0) -> int:
        """The byte at symbol `name` + `offset` (any alignment)."""
        return self.core.memory.u8.raw_read(self.symbol(name) + offset) & 0xFF

    def peek_cstr(self, name: str, maxlen: int = 256) -> str:
        """The NUL-terminated string stored at symbol `name` (at most `maxlen` bytes)."""
        base, out = self.symbol(name), bytearray()
        for i in range(maxlen):
            byte = self.core.memory.u8.raw_read(base + i) & 0xFF
            if byte == 0:
                break
            out.append(byte)
        return out.decode("utf-8", "replace")

    # -- the card -----------------------------------------------------------------
    def flush(self) -> None:
        """Writes the in-memory image back to the .img file (REQUIRED before any list/cat)."""
        self.server.image.flush()

    def snapshot(self) -> Path:
        """Copies the image as it is on disk now to <img>.pre (the 'before' side of report)."""
        pre = self.img.with_name(self.img.name + ".pre")
        shutil.copyfile(self.img, pre)
        self._pre_list = img_list(pre)
        return pre

    def report(self, expect_changed=(), optional=()) -> set:
        """Flushes, diffs the card against the snapshot and returns the changed path set
        (added + removed + modified). Raises RunnerError if a path outside
        expect_changed|optional changed, or an expect_changed path did not."""
        self.flush()
        before = vsd_diff.parse_text(self._pre_list)
        after = vsd_diff.parse_text(img_list(self.img))
        added, removed, changed = vsd_diff.diff_lists(before, after)
        got = set(added) | set(removed) | set(changed)
        expect, opt = set(expect_changed), set(optional)
        unexpected = got - expect - opt
        missing = expect - got
        if unexpected or missing:
            raise RunnerError(f"card diff mismatch: unexpected={sorted(unexpected)} "
                              f"missing={sorted(missing)} (added={added} removed={removed} "
                              f"changed={changed})")
        return got

    def cat(self, path: str) -> bytes:
        """Flushes, then returns the exact bytes of `path` on the card."""
        self.flush()
        return img_cat(self.img, path)

    def listing(self) -> str:
        """Flushes, then returns the card's `vsd_img list` text."""
        self.flush()
        return img_list(self.img)

    @property
    def counters(self) -> dict:
        im, sv = self.server.image, self.server
        return dict(writes_served=im.writes_served, reads_served=im.reads_served,
                    write_fails=im.write_fails, read_fails=im.read_fails,
                    lied_sectors=im.lied_sectors, transactions=sv.transactions_served,
                    unaligned=sv.unaligned_count, frames=self.frames)


def _cli(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", type=Path)
    ap.add_argument("img", type=Path)
    ap.add_argument("--frames", type=int, default=180, help="settle frames before the keys")
    ap.add_argument("--keys", default="", help='comma list, "+" chords: "START,A,DOWN+A"')
    ap.add_argument("--shot", type=Path)
    ap.add_argument("--magic", default=vsd.VSD_LOCATOR_MAGIC.decode())
    ap.add_argument("--protect", action="store_true")
    ap.add_argument("--fail-all-writes", action="store_true")
    ap.add_argument("--lie-writes", action="store_true")
    for knob in ("fail-write-in", "fail-at", "lie-after", "fail-read-at", "fail-reads-after"):
        ap.add_argument(f"--{knob}", type=int)
    args = ap.parse_args(argv)

    knobs = {k: getattr(args, k) for k in
             ("protect", "fail_all_writes", "lie_writes", "fail_write_in", "fail_at",
              "lie_after", "fail_read_at", "fail_reads_after") if getattr(args, k)}
    if args.fail_at is not None:
        knobs["fail_at"] = args.fail_at
    if args.lie_after is not None:
        knobs["lie_after"] = args.lie_after
    runner = Runner(args.rom, args.img, magic=args.magic.encode("ascii"), **knobs)
    runner.run(args.frames)
    for tok in [t for t in args.keys.split(",") if t]:
        runner.tap(tok)
    if args.shot:
        print(f"shot: {runner.shot(args.shot)}")
    runner.flush()
    print(f"counters: {runner.counters}")
    return 0


if __name__ == "__main__":
    sys.exit(_cli())
