#include "sys.h"

#include "flashcartio.h"
#include "flashcartio_write.h"

#ifdef VSD_ENABLE
#include "vsd.h"   /* the harness-hosted virtual SD, emulator-build-only seam */
#endif

#if FLASHCARTIO_EZFO_ENABLE != 0
#include "ezflashomega/io_ezfo.h"
#endif

bool flashcartio_write_sector(u32 sector, const u8* source, u16 count) {
#ifdef VSD_ENABLE
  if (vsd_attached()) {
    /* A source inside ROM (0x08000000..0x0DFFFFFF) would write the bootloader to a real
     * Omega card (f_write from a ROM-resident buffer); refuse it here so the harness
     * surfaces the bug instead of masking it. lib/fatfs/diskio*.c stay untouched. */
    if ((u32)source >= 0x08000000u && (u32)source <= 0x0DFFFFFFu) return false;
    flashcartio_is_reading = true;   /* exercise the same OS-mode gate real writes do */
    bool ok = vsd_xfer(VSD_OP_WRITE, sector, (u32)source, count);
    flashcartio_is_reading = false;
    return ok;
  }
#endif
  switch (active_flashcart) {
#if FLASHCARTIO_EZFO_ENABLE != 0
    case EZ_FLASH_OMEGA: {
      /* Same guard reads use: blocks SoftReset / ROM-touching IRQs while the
       * cart is in OS mode. _EZFO_writeSectors disables IRQs internally too. */
      flashcartio_is_reading = true;
      bool success = _EZFO_writeSectors(sector, count, source);
      flashcartio_is_reading = false;
      return success;
    }
#endif
    default:
      /* Everdrive write not implemented in this project. */
      return false;
  }
}
