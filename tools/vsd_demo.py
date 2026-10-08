#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""vsd_demo.py -- materialise the demo card tree for the README screenshots.

    python3 tools/vsd_demo.py expand TREE_DIR

TREE_DIR gets every file of tools/vsd_demo/manifest.txt as a SPARSE zero-filled file of
the listed size (os.truncate after create; the bodies are never real data), the empty
directories the manifest names, and then tools/vsd_demo/overlay/ copied on top (the few
files with real bytes: configs, notes, pins.txt, shortcuts.txt). Build the card from it
with `vsd_img mkimg CARD.img 320 TREE_DIR file_browser_gba`.

Manifest lines: `<size>\\t<path>` (a file), `#dir\\t<path>` (an empty directory); any other
line starting with `#` is a comment.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEMO = HERE / "vsd_demo"
MANIFEST = DEMO / "manifest.txt"
OVERLAY = DEMO / "overlay"


def parse_manifest(text: str) -> "tuple[list[tuple[int, str]], list[str]]":
    """Returns ([(size, path)...], [empty dir paths...]); raises ValueError on a bad line."""
    files: "list[tuple[int, str]]" = []
    dirs: "list[str]" = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        if line.startswith("#dir\t"):
            dirs.append(line.split("\t", 1)[1])
        elif line.startswith("#"):
            continue
        else:
            size, _, path = line.partition("\t")
            if not size.isdigit() or not path or path.startswith("/") or ".." in path.split("/"):
                raise ValueError(f"manifest line {number}: {line!r}")
            files.append((int(size), path))
    return files, dirs


def expand(tree: Path) -> "tuple[int, int]":
    """Builds the tree under `tree` (must not exist or be empty); returns (files, bytes)."""
    if tree.exists() and any(tree.iterdir()):
        raise SystemExit(f"{tree} is not empty")
    files, dirs = parse_manifest(MANIFEST.read_text(encoding="utf-8"))
    total = 0
    for size, rel in files:
        dest = tree / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        with open(dest, "wb") as handle:
            handle.truncate(size)            # sparse: zero bodies, no blocks on APFS
        total += size
    for rel in dirs:
        (tree / rel).mkdir(parents=True, exist_ok=True)
    shutil.copytree(OVERLAY, tree, dirs_exist_ok=True)
    return len(files), total


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    exp = sub.add_parser("expand", help="materialise the manifest + overlay as a host tree")
    exp.add_argument("tree", type=Path)
    args = ap.parse_args(argv)
    count, total = expand(args.tree)
    print(f"expanded {count} files, {total / 1048576:.1f} MiB (sparse) into {args.tree}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
