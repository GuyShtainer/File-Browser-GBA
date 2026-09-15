#ifndef FLASHCARTIO_H
#define FLASHCARTIO_H

#include <stdbool.h>
#include "fatfs/ff.h"

typedef enum { NO_FLASHCART, EVERDRIVE_GBA_X5, EZ_FLASH_OMEGA } ActiveFlashcart;

extern ActiveFlashcart active_flashcart;
extern volatile bool flashcartio_is_reading;

bool flashcartio_activate(void);

/* What the last flashcartio_activate() concluded, for the boot screen and the log. The
 * value that matters in the field is the EZ-Flash page: 0x200 means the image was
 * SD-loaded into PSRAM, anything below 0x200 means it was booted from the game NOR
 * (page * 128 KiB), and a non-zero look-alike count means a stale build with the same
 * title is still sitting on the cart and was (correctly) rejected. */
#define FCIO_DET_NONE          0   /* activate never ran                                  */
#define FCIO_DET_ED_OK         1   /* EverDrive GBA X5, SD initialised                    */
#define FCIO_DET_ED_SD_FAIL    2   /* EverDrive answered the key probe; its SD init failed */
#define FCIO_DET_EZFO_OK       3   /* EZ-Flash, running image located (content-verified)  */
#define FCIO_DET_EZFO_HDRONLY  4   /* EZ-Flash, located by header word only (bus unstable) */
#define FCIO_DET_EZFO_NOT      5   /* no cart answered: unmapping the ROM changed nothing  */
#define FCIO_DET_EZFO_NOPAGE   6   /* EZ-Flash answered, but no page shows this image      */
#define FCIO_DET_NO_BACKEND    7   /* no backend compiled in                              */
int      flashcartio_detect_code(void);
unsigned flashcartio_ezfo_page(void);                 /* 0x200 PSRAM, <0x200 NOR, 0xFFFF none */
unsigned flashcartio_ezfo_lookalikes(unsigned* first);/* count; *first = first rejected page  */
bool flashcartio_read_sector(unsigned int sector,
                             unsigned char* destination,
                             unsigned short count);

/* Reboot toward the flashcart's loader/menu (EZ-Flash Omega) or OS (EverDrive),
 * falling back to a BIOS SoftReset with no cart. Does not return. EXPERIMENTAL:
 * whether it reaches the cart MENU vs just restarts is hardware-dependent. Call
 * only when idle (no SD transfer in flight); the tool writes no SRAM, so no data
 * is at risk. */
void flashcartio_reboot(void);

#endif  // FLASHCARTIO_H
