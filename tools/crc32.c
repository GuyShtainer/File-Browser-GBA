/* crc32 -- see crc32.h. Host-only, zlib-compatible. */
#include "crc32.h"

static uint32_t s_table[256];
static int      s_table_ready;

static void crc32_build_table(void) {
  uint32_t n;
  for (n = 0; n < 256u; n++) {
    uint32_t c = n;
    int k;
    for (k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    s_table[n] = c;
  }
  s_table_ready = 1;
}

uint32_t crc32_update(uint32_t crc, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  size_t i;
  if (!s_table_ready) crc32_build_table();
  if (p == NULL && len != 0) return crc;   /* refuse a NULL buffer with a length */
  crc = ~crc;
  for (i = 0; i < len; i++) crc = s_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}
