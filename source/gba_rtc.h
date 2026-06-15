#ifndef GBA_RTC_H
#define GBA_RTC_H

#include <stdint.h>
#include <stdbool.h>

/* A decoded wall-clock reading from the cartridge RTC. */
typedef struct {
  uint16_t year;    /* full year, e.g. 2026 */
  uint8_t  month;   /* 1..12 */
  uint8_t  day;     /* 1..31 */
  uint8_t  hour;    /* 0..23 */
  uint8_t  minute;  /* 0..59 */
  uint8_t  second;  /* 0..59 */
} GbaRtcTime;

/* Read the GBA cartridge real-time clock (Seiko S-3511A) over the gamepak GPIO
 * port at 0x080000C4. The EZ-Flash Omega DE emulates this RTC. Returns true and
 * fills *out on a plausible reading, false if the value is out of range — which
 * is what happens if the flashcart doesn't expose its RTC to homebrew. */
bool gba_rtc_get(GbaRtcTime* out);

/* Decoded S-3511A control/status register (read via CMD_STATUS_READ = 0x63). */
typedef struct {
  uint8_t raw;        /* the raw status byte, for logging/diagnostics       */
  bool    power_lost; /* bit 7 (0x80): the RTC lost power since it was last  */
                      /*   set — the backup coin cell drained (or the clock  */
                      /*   was never initialised). The time is unreliable.   */
  bool    mode_24h;   /* bit 6 (0x40): 1 = 24-hour mode                      */
} GbaRtcStatus;

/* Read ONLY the S-3511A control/status register (a single CMD_STATUS_READ
 * transaction) and decode it. This is the one battery-health signal GBA-family
 * hardware exposes: bit 7 reports that the RTC's backup cell lost power. Returns
 * true and fills *out on a plausible read; false for an all-ones (0xFF) read —
 * the open-bus value a flashcart returns when it does not expose its RTC to this
 * ROM. Does NOT modify the clock.
 *
 * The authoritative "is the RTC exposed at all" test remains gba_rtc_get()'s
 * date/time range check, so treat `power_lost` as meaningful only when
 * gba_rtc_get() also succeeds for the same read. */
bool gba_rtc_get_status(GbaRtcStatus* out);

#endif /* GBA_RTC_H */
