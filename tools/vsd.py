#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""vsd.py -- the harness-hosted virtual SD server (tools/vsd.py).

Serves lib/vsd.c's 32-byte EWRAM mailbox (VsdBox) out of a flat, sector-addressed
FAT16 image built by tools/vsd_img.c, one transaction per emulated frame. This module
has no `main()` of its own for mGBA: it is imported by tools/vsd_run.py's Runner (or a
tool's own shot harness) and driven from there. `python3 tools/vsd.py IMG.img` is a
standalone smoke-test CLI for the image half only (does not touch mGBA).

LOCATING THE MAILBOX WITHOUT AN ELF
-----------------------------------
lib/vsd.c defines a `const volatile` locator record, g_vsd_rec: an 8-byte ASCII magic
("GBTKVSD1") immediately followed by two little-endian u32s -- the mailbox's RUNTIME
EWRAM address (a compile-time constant: `&s_vsd`) and sizeof(VsdBox). Scanning the raw
.gba bytes for that magic and reading the 16 bytes after it gives the exact address to
poke through mgba's core.memory, with no `nm`/ELF involved. The magic is a PARAMETER of
find_mailbox()/attach_server(): a project that vendored an older copy with its own
magic (PokeDNA keeps "PDNAVSD1" until it adopts the toolkit copy) passes it explicitly.

PROTOCOL (mirrors lib/vsd.c's vsd_attach()/vsd_xfer() exactly -- read those first)
-----------------------------------------------------------------------------------
1. The GBA side writes magic/op/sector/count/addr/status via vsd_attach() or
   vsd_xfer(), THEN publishes the doorbell (`seq`) LAST -- the host never observes a
   half-formed request.
2. service() is called once per emulated frame, AFTER core.run_frame() (the CPU is
   stopped for the whole call -- which is exactly why no barriers/volatile discipline
   is needed on the host side, and exactly why this harness *cannot* catch a real race;
   admitted rather than claimed as coverage).
3. If magic is right and seq changed since the last serve: op==VSD_OP_NONE (a value
   vsd_xfer() can never publish with a NEW seq -- it early-returns before touching the
   mailbox on any other op) means this is the ATTACH handshake -- just ring the
   doorbell back. Otherwise it is a real transaction: READ copies image bytes into
   GBA memory at `addr`; WRITE copies GBA memory at `addr` into the image. Both use
   core.memory.u32.raw_write/raw_read in 4-byte chunks (raw_write, never the buggy
   __setitem__ binding), falling back to per-byte u8 access for the trailing <4 bytes
   or when `addr` itself is not 4-aligned (counted in `unaligned_count`).

ROM-SOURCED WRITES ARE A HARD HARNESS ERROR
-------------------------------------------
Any WRITE whose `addr` (the caller's SOURCE buffer) falls inside 0x08000000..
0x0DFFFFFF raises VsdRomSourceError immediately, rather than being served. On a real
Omega this is the rom-load-lab bug class: f_write from a ROM-resident buffer silently
writes the BOOTLOADER to the card. lib/flashcartio_write.c refuses such a source on the
GBA side too (the mailbox `addr` is only ever a ROM address for an aligned ROM source).

FAILURE INJECTION -- tests/hostfat/ramdisk.c's own knob set, one layer further out, so
they are drivable through the app's REAL screens. Every N below counts what ramdisk.c
itself counts -- SECTORS for fail_at/lie_after/fail_read_at (a multi-sector FatFs call
can cross the threshold mid-call), CALLS for fail_write_in/fail_reads_after:
  protect          every WRITE fails, unconditionally, forever (a write-protected
                   volume -- see VsdImage's docstring for the one genuine gap: this
                   cannot reach FatFs' separate FR_WRITE_PROTECTED disk_status() path)
  fail_all_writes  every WRITE fails, forever (an EverDrive, by design)
  fail_write_in N  the NEXT N served WRITE calls all fail, then heal (a RUN of N)
  fail_at N        after N successful write SECTORS, the write call that crosses that
                   threshold fails once, then heals
  lie_after N      after N successful write SECTORS, every WRITE reports OK and keeps
                   nothing forever (the card that ACKs and stores nothing)
  lie_writes       every WRITE reports OK and keeps nothing, from the first call
  fail_read_at N   after N successful read SECTORS, the read call that crosses that
                   threshold fails once, then heals
  fail_reads_after N  after N successful read CALLS, every read fails forever
                   (lets a test sweep a read error across every step of a flush)
"""
from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from dataclasses import fields as dc_fields
from pathlib import Path

# ---- VsdBox field offsets (lib/vsd.h) -----------------------------------------
OFF_MAGIC, OFF_SEQ, OFF_OP, OFF_SECTOR, OFF_COUNT, OFF_ADDR, OFF_STATUS, OFF_ACK = (
    0, 4, 8, 12, 16, 20, 24, 28,
)
VSD_MAGIC = 0x31445356
VSD_OP_NONE, VSD_OP_READ, VSD_OP_WRITE = 0, 1, 2
VSD_ST_BUSY, VSD_ST_OK, VSD_ST_ERR = 0, 1, 2

VSD_LOCATOR_MAGIC = b"GBTKVSD1"   # default; override via the `magic=` parameter
ROM_LO, ROM_HI = 0x08000000, 0x0DFFFFFF   # the ROM-source refusal range
SECTOR = 512


class VsdError(Exception):
    """Any harness-side protocol/config error -- never silently swallowed (golden
    rule 3: fail fast and loudly)."""


class VsdRomSourceError(VsdError):
    """A disk_write whose source address is inside ROM. On real hardware this
    writes the bootloader to the card; the server refuses to simulate it."""


def find_mailbox(rom_bytes: bytes, magic: bytes = VSD_LOCATOR_MAGIC) -> tuple[int, int]:
    """Scans `rom_bytes` (a .gba image) for the 8-byte locator `magic` (lib/vsd.c's
    g_vsd_rec) and returns (mailbox_addr, mailbox_size). Raises VsdError if it is
    missing, ambiguous (two hits are a hard setup error, never guessed at), misaligned,
    or carries a size other than 32 (a stale build or a false-positive hit would
    otherwise be served through the wrong fields)."""
    if len(magic) != 8:
        raise VsdError(f"locator magic must be 8 bytes, got {len(magic)}")
    name = magic.decode("ascii", "replace")
    hits = []
    start = 0
    while True:
        i = rom_bytes.find(magic, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if not hits:
        raise VsdError(
            f"no {name} locator record found -- this build was not made with "
            "VSD_ENABLE (or carries a different locator magic)")
    if len(hits) > 1:
        raise VsdError(
            f"{len(hits)} {name} occurrences at "
            f"{', '.join(hex(h) for h in hits)} -- ambiguous, refusing to guess")
    off = hits[0]
    if off % 4:
        raise VsdError(f"{name} locator record at 0x{off:X} is not 4-byte aligned")
    addr, size = struct.unpack_from("<II", rom_bytes, off + 8)
    if size != 32:
        raise VsdError(f"{name} locator record at 0x{off:X} claims size={size}, "
                        f"expected 32 (sizeof(VsdBox)) -- stale build or a false hit")
    return addr, size


@dataclass
class VsdImage:
    """A flat, sector-addressed FAT16 volume file (no partition table -- tools/vsd_img.c
    builds it with FM_SFD, matching disk_ioctl's GET_SECTOR_SIZE=512). Held entirely in
    memory (`bytearray`) and flushed back to disk on demand; a chain never has more than
    a few MiB of image, so this is simpler and faster than seeking a file handle per
    transaction.

    Implements tests/hostfat/ramdisk.c's disk_read()/disk_write() failure-injection
    checks VERBATIM  -- same knob names, same
    per-call check order, same sector-vs-call counting per knob -- so the same fault
    vocabulary the host tests already use is drivable through mGBA.

    ONE genuine gap, not "verbatim": ramdisk.c's `rd_protect` is read by disk_STATUS()
    too (`disk_status` returns STA_PROTECT), which is what makes FatFs' f_open()
    refuse with FR_WRITE_PROTECTED *before* ever calling disk_write() at all
    (ff.c:3417-3418/3432-3433). The VSD wire protocol has no STATUS op and no third
    VsdBox status code to carry that distinction (VSD_ST_BUSY/OK/ERR only, source/
    vsd.h) -- and neither, on THIS project, does real hardware: lib/fatfs/diskio.c's
    disk_status() is `return driveId == 0 ? 0 : STA_NOINIT;`, unconditionally, on
    every build including the shipped Omega one, so STA_PROTECT/FR_WRITE_PROTECTED
    has never once fired anywhere in this codebase, real card or virtual. `protect`
    here therefore mirrors disk_write()'s OWN protect check (`if (rd_protect) return
    RES_WRPRT;`) -- every write fails -- exactly like every other write-fail knob
    (VSD_ST_ERR, not a WRPRT-specific code that nothing downstream of it, real or
    virtual, has ever distinguished from any other write error).
    """

    path: Path
    data: bytearray
    sectors: int

    # failure injection knobs (all off by default). Every
    # knob below is now SECTOR-counted, matching tests/hostfat/ramdisk.c's own
    # disk_read/disk_write exactly (`rd_fail_at`/`rd_lie_after`/`rd_fail_read_at` all
    # decrement by the served CALL's `count`, not by 1 -- the earlier per-CALL
    # decrement meant "the Nth write" actually meant "the Nth disk_write CALL",
    # which silently changes meaning every time FatFs batches a different number of
    # sectors into one call). `fail_write_in`/`fail_reads_after` stay CALL-counted --
    # that is what ramdisk.c itself does for those two (`rd_fail_write_in--`/
    # `rd_fail_reads_after--`, both by 1, never by `count`).
    protect: bool = False           # rd_protect: every write fails (mirrors RES_WRPRT's
                                     # EFFECT -- see this class's own docstring note on
                                     # WHY it cannot mirror the CODE, below)
    fail_all_writes: bool = False   # rd_fail_all_writes: every write fails, unconditionally
    fail_write_in: int = 0          # rd_fail_write_in: >0, fails that many CALLS from now
    fail_at: int = -1               # rd_fail_at: >=0, let that many SECTORS land, fail one call
    lie_after: int = -1             # rd_lie_after: >=0, let that many SECTORS land, then lie forever
    lie_writes: bool = False        # rd_lie_writes: every write lies from the very first call
    lying: bool = False             # derived: set True once lie_after's countdown reaches it
    fail_reads_after: int = -1      # rd_fail_reads_after: >=0, let that many CALLS through,
                                     # then fail every read forever (never heals)
    fail_read_at: int = -1          # rd_fail_read_at: >=0, let that many SECTORS land, fail one call

    # counters (mirror ramdisk.c's rd_* globals)
    writes_served: int = 0
    reads_served: int = 0
    write_fails: int = 0
    read_fails: int = 0
    lied_sectors: int = 0

    @classmethod
    def load(cls, path: Path) -> "VsdImage":
        raw = Path(path).read_bytes()
        if len(raw) % SECTOR != 0:
            raise VsdError(f"{path}: not sector-aligned ({len(raw)} bytes)")
        return cls(path=Path(path), data=bytearray(raw), sectors=len(raw) // SECTOR)

    def flush(self, out_path: "Path | None" = None) -> None:
        Path(out_path or self.path).write_bytes(bytes(self.data))

    def read(self, sector: int, count: int) -> "bytes | None":
        """Returns the sector data, or None if a read-fail knob fired (mirrors
        ramdisk.c's disk_read returning RES_ERROR -- the caller maps None to
        VSD_ST_ERR). Check order mirrors disk_read() exactly: fail_reads_after (call-
        counted, never heals) before fail_read_at (sector-counted, heals once)."""
        if sector + count > self.sectors:
            raise VsdError(f"read out of range: sector={sector} count={count} "
                            f"sectors={self.sectors}")
        if self.fail_reads_after >= 0:
            if self.fail_reads_after == 0:
                self.read_fails += 1
                return None
            self.fail_reads_after -= 1
        if self.fail_read_at >= 0:
            if self.fail_read_at < count:
                self.fail_read_at = -1
                self.read_fails += 1
                return None
            self.fail_read_at -= count
        self.reads_served += 1
        off = sector * SECTOR
        return bytes(self.data[off:off + count * SECTOR])

    def write(self, sector: int, payload: bytes) -> bool:
        """Returns False if the write was refused/lost (mirrors ramdisk.c's disk_write
        returning RES_ERROR, or RES_OK-but-discarded for a lying knob -- the CALLER
        still sees VSD_ST_OK for a lie, exactly like a real card that ACKs and keeps
        nothing; that asymmetry is the whole point of --vsd-lie-after). Check
        order mirrors disk_write() exactly: protect/fail_all_writes/fail_write_in
        (all unconditional or call-counted) before the sector-counted fail_at, before
        lie_after/lie_writes."""
        if len(payload) % SECTOR:
            raise VsdError(f"write payload not sector-sized ({len(payload)} bytes)")
        count = len(payload) // SECTOR
        if sector + count > self.sectors:
            raise VsdError(f"write out of range: sector={sector} count={count} "
                            f"sectors={self.sectors}")
        if self.protect:
            self.write_fails += 1
            return False
        if self.fail_all_writes:
            self.write_fails += 1
            return False
        if self.fail_write_in > 0:
            self.fail_write_in -= 1
            self.write_fails += 1
            return False
        if self.fail_at >= 0:
            if self.fail_at < count:
                self.fail_at = -1
                self.write_fails += 1
                return False
            self.fail_at -= count
        if self.lie_after >= 0:
            if self.lie_after == 0:
                self.lying = True
            else:
                self.lie_after -= count
                if self.lie_after < 0:
                    self.lie_after = 0
        if self.lie_writes or self.lying:
            self.lied_sectors += count
            return True   # ACKs, keeps nothing
        off = sector * SECTOR
        self.data[off:off + len(payload)] = payload
        self.writes_served += count
        return True


class VsdServer:
    """Owns the mGBA `core`, the mailbox address, and a VsdImage. Call `service()`
    once per emulated frame, strictly AFTER `core.run_frame()` -- the whole
    no-barriers argument depends on the CPU being stopped for the duration of the
    call."""

    def __init__(self, core, mailbox_addr: int, image: VsdImage):
        self.core = core
        self.mailbox = mailbox_addr
        self.image = image
        self.last_seq_acked = 0
        self.attached = False
        # Served transfers whose GBA-side address was not 4-aligned. Structurally 0 when
        # diskio stages an unaligned CALLER buffer through an aligned bounce buffer
        # before the mailbox `addr` is written; a non-zero count is a product bug.
        self.unaligned_count = 0
        self.transactions_served = 0
        self.roundtrip_disk_write_calls = 0   # every served READ or WRITE, incl. attach

    def _read_bytes(self, addr: int, n: int) -> bytes:
        u32 = self.core.memory.u32
        u8 = self.core.memory.u8
        out = bytearray(n)
        i = 0
        if addr % 4 == 0:
            while i + 4 <= n:
                v = u32.raw_read(addr + i) & 0xFFFFFFFF
                out[i:i + 4] = v.to_bytes(4, "little")
                i += 4
        while i < n:
            out[i] = u8.raw_read(addr + i) & 0xFF
            i += 1
        return bytes(out)

    def _write_bytes(self, addr: int, payload: bytes) -> None:
        u32 = self.core.memory.u32
        u8 = self.core.memory.u8
        n = len(payload)
        i = 0
        if addr % 4 == 0:
            while i + 4 <= n:
                v = int.from_bytes(payload[i:i + 4], "little")
                u32.raw_write(addr + i, v)
                i += 4
        while i < n:
            u8.raw_write(addr + i, payload[i])
            i += 1

    def service(self) -> bool:
        """Returns True iff a transaction (attach handshake or real I/O) was served
        this call. Raises VsdRomSourceError on a ROM-source hit -- callers let this
        propagate; it is a harness bug class, not a recoverable condition."""
        u32 = self.core.memory.u32
        magic = u32.raw_read(self.mailbox + OFF_MAGIC) & 0xFFFFFFFF
        if magic != VSD_MAGIC:
            return False
        seq = u32.raw_read(self.mailbox + OFF_SEQ) & 0xFFFFFFFF
        if seq == self.last_seq_acked:
            return False

        op = u32.raw_read(self.mailbox + OFF_OP) & 0xFFFFFFFF

        if op == VSD_OP_NONE:
            # vsd_xfer() never publishes a NEW seq with op==NONE (it early-returns
            # before touching the mailbox for any op other than READ/WRITE) -- the
            # only way to reach here is vsd_attach()'s own initial handshake.
            u32.raw_write(self.mailbox + OFF_STATUS, VSD_ST_OK)
            u32.raw_write(self.mailbox + OFF_ACK, seq)
            self.last_seq_acked = seq
            self.attached = True
            return True

        sector = u32.raw_read(self.mailbox + OFF_SECTOR) & 0xFFFFFFFF
        count = u32.raw_read(self.mailbox + OFF_COUNT) & 0xFFFFFFFF
        addr = u32.raw_read(self.mailbox + OFF_ADDR) & 0xFFFFFFFF

        if addr % 4 != 0:
            self.unaligned_count += 1

        status = VSD_ST_ERR
        if op == VSD_OP_READ:
            data = self.image.read(sector, count)
            if data is not None:
                self._write_bytes(addr, data)
                status = VSD_ST_OK
        elif op == VSD_OP_WRITE:
            if ROM_LO <= addr <= ROM_HI:
                raise VsdRomSourceError(
                    f"disk_write source addr=0x{addr:08x} is inside ROM "
                    f"(0x{ROM_LO:08x}..0x{ROM_HI:08x}) -- hard harness error: "
                    "on a real Omega this writes the BOOTLOADER to the card")
            payload = self._read_bytes(addr, count * SECTOR)
            status = VSD_ST_OK if self.image.write(sector, payload) else VSD_ST_ERR
        # else: unknown op -- status stays VSD_ST_ERR, still acked below so the GBA
        # side's spin loop is released rather than left to time out.

        u32.raw_write(self.mailbox + OFF_STATUS, status)
        u32.raw_write(self.mailbox + OFF_ACK, seq)
        self.last_seq_acked = seq
        self.transactions_served += 1
        self.roundtrip_disk_write_calls += 1
        return True


def attach_server(core, rom_bytes: bytes, img_path: Path,
                  magic: bytes = VSD_LOCATOR_MAGIC, **knobs) -> VsdServer:
    """Convenience: locate the mailbox (locator `magic`, default "GBTKVSD1") in
    `rom_bytes`, load `img_path`, build a VsdServer, and apply any failure-injection
    knob passed as a kwarg (e.g. `protect=True`, `fail_write_in=3`). Raises VsdError
    on an unknown knob name. Used by tools/vsd_run.py."""
    addr, _size = find_mailbox(rom_bytes, magic)
    image = VsdImage.load(img_path)
    # A mistyped or renamed knob (e.g. a caller still passing `fail_at_n=` after a rename) used to silently become a NEW, dead
    # instance attribute via plain setattr() -- the actual VsdImage field it meant to
    # set stayed at its default, and the caller's whole failure-injection scenario
    # never fired, with no error anywhere. Checked against the DATACLASS FIELD names
    # specifically (dataclasses.fields(), not hasattr()) -- hasattr() would also be
    # True for `read`/`write`/`flush` (bound methods), and setattr()ing one of those
    # would silently replace a method with whatever value a caller passed.
    field_names = {f.name for f in dc_fields(image)}
    for k, v in knobs.items():
        if v is None:
            continue
        if k not in field_names:
            raise VsdError(f"attach_server(): unknown VSD knob {k!r} -- not a "
                            f"VsdImage field")
        setattr(image, k, v)
    return VsdServer(core, addr, image)


def _cli(argv: "list[str] | None" = None) -> int:
    """Standalone smoke test of the image half only (no mGBA): loads an image, prints
    its sector count and the mailbox-locator search result against a .gba if given."""
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path, help="a tools/vsd_img.c mkimg .img file")
    ap.add_argument("--gba", type=Path, help="optionally also locate the mailbox in this .gba")
    args = ap.parse_args(argv)

    img = VsdImage.load(args.image)
    print(f"{args.image}: {img.sectors} sectors ({img.sectors * SECTOR} bytes)")
    if args.gba:
        addr, size = find_mailbox(args.gba.read_bytes())
        print(f"{args.gba}: mailbox at 0x{addr:08x}, size {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(_cli())
