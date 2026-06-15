#ifndef IO_EZFO_H
#define IO_EZFO_H

#pragma GCC system_header

#include "../sys.h"

bool _EZFO_startUp(void);
bool _EZFO_readSectors(u32 address, u32 count, void* buffer);
bool _EZFO_writeSectors(u32 address, u32 count, const void* buffer);

/* Reboot into the EZ-Flash kernel/menu: map the bootloader page then BIOS
 * SoftReset (jumps to 0x08000000, now the kernel). EWRAM-resident (ROM is
 * unmapped after the rompage switch). Does not return. EXPERIMENTAL — whether
 * it lands on the file menu vs just restarts needs hardware validation. */
void _EZFO_reboot(void);

#endif /* IO_EZFO_H */
