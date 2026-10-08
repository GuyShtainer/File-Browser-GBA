/*
 * vsd -- the harness-hosted "virtual SD" mailbox. See vsd.h for the contract and
 * docs/kb/virtual-sd-harness.md for the full protocol. The whole body sits under
 * #ifdef VSD_ENABLE -- no symbol from this file exists in a shipped build.
 */
#include "vsd.h"

#ifdef VSD_ENABLE

#include <tonc.h>          /* REG_VCOUNT, EWRAM_BSS */
#include "flashcartio.h"   /* active_flashcart, EZ_FLASH_OMEGA */

/* 32 B, exactly VsdBox -- the wire protocol the host reads/writes directly through
 * core.memory while the emulated CPU is stopped (so no barriers are needed host-side).
 * Lives in .sbss, like every other emulator-build-only static. */
static VsdBox EWRAM_BSS s_vsd;

/* Latched false forever after a timeout; vsd_xfer() checks this before ever touching
 * s_vsd so a dead harness costs one instruction, not a spin. */
static bool s_attached;

/* The spin bodies increment this volatile counter. s_vsd.ack is already volatile, so
 * the counter is not what makes the loop correct; it keeps mGBA's idle-loop
 * fast-forward from treating a loop that touches no other memory as idle and skipping
 * emulated time past the frame boundary the host needs to observe. Kept outside VsdBox
 * on purpose: VsdBox is the wire protocol the host parses by field offset. */
static volatile uint32_t s_spin_count;

/* Locator record: `const volatile` + `used` so a byte scanner (no ELF, no symbol
 * table) can find the mailbox's runtime address in the raw .gba image. */
const volatile VsdRec g_vsd_rec __attribute__((used, aligned(4))) = {
  { 'G', 'B', 'T', 'K', 'V', 'S', 'D', '1' }, (uint32_t)&s_vsd, (uint32_t)sizeof(VsdBox)
};

/* One VCOUNT wrap (a large value dropping to a small one, e.g. 227->0) counts as "one
 * frame elapsed" -- deterministic regardless of build speed, unlike an iteration-count
 * bound, which would depend on how fast the spin body runs. */
static int vsd_wrap_frame(uint16_t* last_vc) {
  uint16_t vc = REG_VCOUNT;
  int wrapped = (vc < *last_vc) ? 1 : 0;
  *last_vc = vc;
  return wrapped;
}

bool vsd_attach(void) {
  /* Force a genuine RUNTIME reference to g_vsd_rec. `used` stops the compiler
   * discarding an unreferenced global, but --gc-sections still discards the whole
   * SECTION unless something reachable from main() reads it; the HOST, not the GBA,
   * is this record's only consumer, so without this read `nm` shows no g_vsd_rec and
   * the host's find_mailbox() cannot locate the mailbox. Both sides are the SAME
   * compile-time constant (&s_vsd), so the check can never legitimately fail; if it
   * does, the section layout changed underneath the record and s_attached must stay
   * false rather than hand the host a stale/wrong address. */
  if (g_vsd_rec.addr != (uint32_t)&s_vsd) {
    s_attached = false;
    return false;
  }

  s_vsd.op     = VSD_OP_NONE;
  s_vsd.sector = 0;
  s_vsd.count  = 0;
  s_vsd.addr   = 0;
  s_vsd.ack    = 0;
  s_vsd.status = VSD_ST_BUSY;
  s_vsd.seq    = 1;         /* the doorbell: written after every other field above */
  s_vsd.magic  = VSD_MAGIC; /* written LAST of the handshake fields, first thing the
                              * host looks for -- the harness must not act on a
                              * mailbox whose seq/status it saw before magic landed */

  uint16_t last_vc = REG_VCOUNT;
  uint32_t frames = 0;
  while (s_vsd.ack != s_vsd.seq) {
    s_spin_count++;
    if (vsd_wrap_frame(&last_vc)) {
      frames++;
      if (frames >= 4) {
        /* Same degrade as vsd_xfer's timeout below: a timed-out VSD path always
         * leaves active_flashcart == NO_FLASHCART (set explicitly so a future edit
         * that moves the success-path assignment earlier cannot reopen the real-EZFO
         * fallback), and the mailbox is disowned so a host that starts serving late
         * is never answered by a GBA side that has already moved on. */
        s_attached = false;
        active_flashcart = NO_FLASHCART;
        s_vsd.magic = 0;
        return false;
      }
    }
  }
  s_attached = true;
  active_flashcart = EZ_FLASH_OMEGA;
  return true;
}

bool vsd_attached(void) { return s_attached; }

bool vsd_xfer(uint32_t op, uint32_t sector, uint32_t addr, uint32_t count) {
  if (!s_attached) return false;
  if (op != VSD_OP_READ && op != VSD_OP_WRITE) return false;

  s_vsd.op     = op;
  s_vsd.sector = sector;
  s_vsd.count  = count;
  s_vsd.addr   = addr;
  s_vsd.status = VSD_ST_BUSY;
  s_vsd.seq    = s_vsd.seq + 1;   /* doorbell last */

  uint16_t last_vc = REG_VCOUNT;
  uint32_t frames = 0;
  while (s_vsd.ack != s_vsd.seq) {
    s_spin_count++;
    if (vsd_wrap_frame(&last_vc)) {
      frames++;
      if (frames >= 16) {
        /* A timed-out transfer must not leave active_flashcart == EZ_FLASH_OMEGA (set
         * by vsd_attach()'s success path): the NEXT SD op would fall through
         * flashcartio's dispatch into the REAL _EZFO_readSectors -- Visoly unlock,
         * WAITCNT, DMA from 0x09xxxxxx -- inside an emulator that has no such
         * hardware. Degrade to no card at all, never back to the real driver. */
        s_attached = false;
        active_flashcart = NO_FLASHCART;
        s_vsd.magic = 0;                  /* the host must not serve this abandoned request */
        return false;
      }
    }
  }
  bool ok = (s_vsd.status == VSD_ST_OK);
  /* Mailbox goes idle once served -- reached ONLY on the loop's normal-exit path (the
   * timeout above returns straight out), so a harness watching `op` flip back to
   * VSD_OP_NONE knows the spin loop actually noticed the ack and resumed. Reuses a
   * field already inside the 32-byte mailbox. */
  s_vsd.op = VSD_OP_NONE;
  return ok;
}

#endif /* VSD_ENABLE */
