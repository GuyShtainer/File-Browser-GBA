#ifndef VSD_H
#define VSD_H

#include <stdint.h>
#include <stdbool.h>

/*
 * vsd -- the harness-hosted "virtual SD" mailbox (protocol: see below).
 * A 32-byte EWRAM mailbox that the host (Python driving mGBA through its bindings)
 * serves from a FAT16 image, one transaction per emulated frame. Wired into the
 * flashcartio sector dispatch by exactly two seams (flashcartio_read_sector /
 * flashcartio_write_sector).
 *
 * The whole body sits under #ifdef VSD_ENABLE: VSD_ENABLE is defined only by an
 * emulator-only build target, so no symbol from this module exists in a shipped
 * build -- `nm <tool>.elf | grep -c vsd_` must read 0 there.
 */

#ifdef VSD_ENABLE

#define VSD_MAGIC    0x31445356u   /* 'VSD1', little-endian in memory */
#define VSD_OP_NONE  0u
#define VSD_OP_READ  1u
#define VSD_OP_WRITE 2u
#define VSD_ST_BUSY  0u
#define VSD_ST_OK    1u
#define VSD_ST_ERR   2u

/* 32 bytes, one field per line. The host writes `status` before `ack`; the GBA side
 * writes `seq` LAST so the host never observes a half-formed request. */
typedef struct {
  volatile uint32_t magic;   /* +0  VSD_MAGIC, written once by vsd_attach()      */
  volatile uint32_t seq;     /* +4  ++ on every request; the doorbell            */
  volatile uint32_t op;      /* +8  VSD_OP_NONE | VSD_OP_READ | VSD_OP_WRITE     */
  volatile uint32_t sector;  /* +12 LBA                                          */
  volatile uint32_t count;   /* +16 sectors                                      */
  volatile uint32_t addr;    /* +20 GBA address of the caller's buffer           */
  volatile uint32_t status;  /* +24 host writes it before ack                    */
  volatile uint32_t ack;     /* +28 host echoes seq                              */
} VsdBox;

/* The locator record: a `const volatile` global carrying a unique magic + the
 * mailbox's address, scanned for in the raw `.gba` bytes so the host needs no ELF or
 * symbol table. 16 bytes; `used` plus a runtime read in vsd_attach() keeps it through
 * --gc-sections. */
typedef struct {
  char     magic[8];   /* "GBTKVSD1", fixed width, no NUL */
  uint32_t addr;       /* &s_vsd */
  uint32_t size;       /* sizeof(VsdBox), 32 -- lets a scanner sanity-check the hit */
} VsdRec;

extern const volatile VsdRec g_vsd_rec;

/* Attach handshake: writes magic/op/seq/status, then spins a BOUNDED 4 frames (counted
 * by REG_VCOUNT wraps, not iteration count) waiting for ack == seq. Returns true and
 * sets active_flashcart = EZ_FLASH_OMEGA on success; false (and latches "never
 * attached" forever) on timeout -- every later vsd_xfer() then returns false in one
 * instruction, so an unattended build boots exactly as it would without the harness.
 * Call at most once, from the emulator build's boot branch, instead of
 * flashcartio_activate(). */
bool vsd_attach(void);

/* True once vsd_attach() has succeeded; false forever after a timeout or before the
 * first call. Cheap to call from the flashcartio seams on every transfer. */
bool vsd_attached(void);

/* One transaction: publish the request, ring the doorbell (seq last), spin on ack,
 * bail with false after 16 frames and latch vsd_attached() false so a dead harness
 * degrades rather than hangs. `op` is VSD_OP_READ/VSD_OP_WRITE, `addr` is the GBA
 * address of the caller's buffer (NOT copied into the mailbox itself -- the host
 * reads/writes it directly through core.memory while the CPU is stopped). */
bool vsd_xfer(uint32_t op, uint32_t sector, uint32_t addr, uint32_t count);

#else
typedef int vsd_no_empty_tu;
#endif /* VSD_ENABLE */

#endif /* VSD_H */
