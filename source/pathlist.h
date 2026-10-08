#ifndef PATHLIST_H
#define PATHLIST_H

/*
 * Packed list of absolute paths (pure C: <stdint.h>/<stdbool.h>/<string.h> only,
 * host-tested by tests/host_pathlist_test.c). The paths live NUL-terminated,
 * back to back, in a caller-owned pool, in insertion order. There is no entry
 * count cap of its own (only the pool size bounds it) unless `max_entries` is
 * non-zero (the START-menu shortcuts use 16).
 *
 * Serialised form = one path per line (LF; CRLF accepted on read), which is
 * exactly `used` bytes, since each NUL is replaced by one LF.
 */

#include <stdint.h>
#include <stdbool.h>

#define PL_PATH_MAX 255        /* longest path accepted (excl. NUL) */

typedef struct {
  char*    pool;
  uint32_t cap;                /* pool bytes                      */
  uint32_t used;               /* bytes used (paths + their NULs) */
  int      count;
  int      max_entries;        /* 0 = unlimited                   */
} PathList;

typedef enum { PL_OK = 0, PL_DUP = 1, PL_FULL = -1, PL_BAD = -2 } PlResult;

void        pl_init(PathList* l, char* pool, uint32_t cap, int max_entries);
void        pl_clear(PathList* l);
int         pl_count(const PathList* l);
/* PL_OK added, PL_DUP already present (list unchanged), PL_FULL no room (pool
 * or entry cap), PL_BAD empty / too long / not starting with '/'. */
PlResult    pl_add(PathList* l, const char* path);
int         pl_find(const PathList* l, const char* path);        /* index or -1 */
bool        pl_remove(PathList* l, const char* path);
bool        pl_remove_at(PathList* l, int idx);
const char* pl_get(const PathList* l, int idx);                  /* NULL if out of range */

/* Replace the list with the lines of text[0..len): LF or CRLF, blank and
 * non-absolute lines skipped, duplicates dropped, stops when full. Returns the
 * number of entries now in the list. */
int         pl_parse(PathList* l, const char* text, uint32_t len);
/* Write the list as one path per line. Returns bytes written (== used) or -1
 * if `cap` is too small. No NUL terminator is written. */
int         pl_serialize(const PathList* l, char* out, uint32_t cap);

#endif /* PATHLIST_H */
