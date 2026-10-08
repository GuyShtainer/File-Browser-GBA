#ifndef TOOLS_CRC32_H
#define TOOLS_CRC32_H
/*
 * crc32 -- table-driven CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), bit-compatible
 * with zlib's crc32(). HOST-ONLY (the vsd_img tool and its test); the table is built
 * on first use, so it is not a GBA-side module.
 *
 * Streaming use mirrors zlib: start from 0, feed chunks, the return value of one call
 * is the `crc` argument of the next.
 *     uint32_t c = 0;
 *     c = crc32_update(c, chunk1, n1);
 *     c = crc32_update(c, chunk2, n2);
 * Check vector: crc32_update(0, "123456789", 9) == 0xCBF43926.
 */
#include <stddef.h>
#include <stdint.h>

uint32_t crc32_update(uint32_t crc, const void* data, size_t len);

#endif /* TOOLS_CRC32_H */
