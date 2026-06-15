#ifndef FLASHCARTIO_H
#define FLASHCARTIO_H

#include <stdbool.h>
#include "fatfs/ff.h"

typedef enum { NO_FLASHCART, EVERDRIVE_GBA_X5, EZ_FLASH_OMEGA } ActiveFlashcart;

extern ActiveFlashcart active_flashcart;
extern volatile bool flashcartio_is_reading;

bool flashcartio_activate(void);
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
