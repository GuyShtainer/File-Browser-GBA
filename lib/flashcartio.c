#include "sys.h"

#include "flashcartio.h"

#if FLASHCARTIO_ED_ENABLE != 0
#include "everdrivegbax5/disk.h"
#include "everdrivegbax5/everdrive.h"
#endif

#if FLASHCARTIO_EZFO_ENABLE != 0
#include "ezflashomega/io_ezfo.h"
#endif

#ifdef VSD_ENABLE
#include "vsd.h"   /* the harness-hosted virtual SD, emulator-build-only seam */
#endif

ActiveFlashcart active_flashcart = NO_FLASHCART;
volatile bool flashcartio_is_reading = false;

/* What the last activate concluded (flashcartio.h FCIO_DET_*). Plain static: read from
 * ROM code between transfers, never during one. */
static int s_detect = FCIO_DET_NONE;

int flashcartio_detect_code(void) { return s_detect; }

unsigned flashcartio_ezfo_page(void) {
#if FLASHCARTIO_EZFO_ENABLE != 0
  return _EZFO_rompage();
#else
  return 0xFFFFu;
#endif
}

unsigned flashcartio_ezfo_lookalikes(unsigned* first) {
#if FLASHCARTIO_EZFO_ENABLE != 0
  return _EZFO_lookalikes(first);
#else
  if (first) *first = 0xFFFFu;
  return 0;
#endif
}

bool flashcartio_activate(void) {
#if FLASHCARTIO_ED_ENABLE != 0

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
  u16 ime = REG_IME;
  REG_IME = 0;
#endif

  // Everdrive GBA X5
  if (ed_init_sd_only()) {
    ed_init();
    ed_set_save_type(FLASHCARTIO_ED_SAVE_TYPE);
    bool success = diskInit() == 0;
    ed_lock_regs();
    if (!success) {
      s_detect = FCIO_DET_ED_SD_FAIL;
#if FLASHCARTIO_ED_DISABLE_IRQ != 0
      REG_IME = ime;
#endif

      return false;
    }

    active_flashcart = EVERDRIVE_GBA_X5;
    s_detect = FCIO_DET_ED_OK;

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
    REG_IME = ime;
#endif

    return true;
  }

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
  // the EverDrive probe said "not me": hand IRQs back before the EZ-Flash
  // path, which brackets its own IME save/restore.
  REG_IME = ime;
#endif

#endif

#if FLASHCARTIO_EZFO_ENABLE != 0
  // EZ Flash Omega
  if (_EZFO_startUp()) {
    active_flashcart = EZ_FLASH_OMEGA;
    s_detect = (_EZFO_detect_result() == EZFO_DET_OK_HDRONLY) ? FCIO_DET_EZFO_HDRONLY
                                                               : FCIO_DET_EZFO_OK;
    return true;
  }
  s_detect = (_EZFO_detect_result() == EZFO_DET_NO_PAGE) ? FCIO_DET_EZFO_NOPAGE
                                                          : FCIO_DET_EZFO_NOT;
  return false;
#else
  s_detect = FCIO_DET_NO_BACKEND;
  return false;
#endif
}

bool flashcartio_read_sector(u32 sector, u8* destination, u16 count) {
#ifdef VSD_ENABLE
  if (vsd_attached()) {
    flashcartio_is_reading = true;   /* exercise the same OS-mode gate real reads do */
    bool ok = vsd_xfer(VSD_OP_READ, sector, (u32)destination, count);
    flashcartio_is_reading = false;
    return ok;
  }
#endif
  switch (active_flashcart) {
#if FLASHCARTIO_ED_ENABLE != 0
    case EVERDRIVE_GBA_X5: {
#if FLASHCARTIO_ED_DISABLE_IRQ != 0
      u16 ime = REG_IME;
      REG_IME = 0;
#endif

      flashcartio_is_reading = true;
      ed_unlock_regs();
      bool success = diskRead(sector, destination, count) == 0;
      ed_lock_regs();
      flashcartio_is_reading = false;

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
      REG_IME = ime;
#endif

      return success;
    }
#endif
#if FLASHCARTIO_EZFO_ENABLE != 0
    case EZ_FLASH_OMEGA: {
      flashcartio_is_reading = true;
      bool success = _EZFO_readSectors(sector, count, destination);
      flashcartio_is_reading = false;
      return success;
    }
#endif
    default:
      return false;
  }
}

void flashcartio_reboot(void) {
  if (flashcartio_is_reading) return;  // never reset mid-transfer (rule #1)
  REG_IME = 0;  // we are not coming back; keep IRQs off through the reset
  switch (active_flashcart) {
#if FLASHCARTIO_EZFO_ENABLE != 0
    case EZ_FLASH_OMEGA:
      _EZFO_reboot();  // SetRompage(BOOTLOADER) + SoftReset -> kernel; no return
      return;
#endif
#if FLASHCARTIO_ED_ENABLE != 0
    case EVERDRIVE_GBA_X5:
      // The activate path left the EverDrive registers LOCKED; ed_reboot writes
      // REG_CFG, which is ignored while locked, so unlock first or the write
      // (and thus the reboot) is a no-op. quick_boot=0 => swi 0x26 HardReset,
      // which cold-boots toward the EverDrive OS menu.
      ed_unlock_regs();
      ed_reboot(0);  // no return
      return;
#endif
    default:
      break;
  }
  asm volatile("swi 0x00" ::: "memory");  // no/unknown cart: plain BIOS SoftReset
  for (;;) {}                             // unreachable
}
