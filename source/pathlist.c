/* pathlist.c - packed absolute-path list (see pathlist.h). Pure C. */
#include "pathlist.h"
#include <string.h>

void pl_init(PathList* l, char* pool, uint32_t cap, int max_entries) {
  if (l == NULL) return;
  l->pool = pool;
  l->cap = (pool == NULL) ? 0u : cap;
  l->used = 0;
  l->count = 0;
  l->max_entries = max_entries;
}

void pl_clear(PathList* l) {
  if (l == NULL) return;
  l->used = 0;
  l->count = 0;
}

int pl_count(const PathList* l) { return l ? l->count : 0; }

const char* pl_get(const PathList* l, int idx) {
  uint32_t o = 0;
  int i;
  if (l == NULL || idx < 0 || idx >= l->count) return NULL;
  for (i = 0; i < idx; i++) o += (uint32_t)strlen(l->pool + o) + 1u;   /* bounded by count */
  return l->pool + o;
}

int pl_find(const PathList* l, const char* path) {
  uint32_t o = 0;
  int i;
  if (l == NULL || path == NULL) return -1;
  for (i = 0; i < l->count; i++) {
    if (strcmp(l->pool + o, path) == 0) return i;
    o += (uint32_t)strlen(l->pool + o) + 1u;
  }
  return -1;
}

PlResult pl_add(PathList* l, const char* path) {
  size_t n;
  if (l == NULL || path == NULL || l->pool == NULL) return PL_BAD;
  n = strlen(path);
  if (n == 0 || n > PL_PATH_MAX || path[0] != '/') return PL_BAD;
  if (pl_find(l, path) >= 0) return PL_DUP;
  if ((l->max_entries > 0 && l->count >= l->max_entries) || n + 1u > l->cap - l->used)
    return PL_FULL;
  memcpy(l->pool + l->used, path, n + 1u);
  l->used += (uint32_t)n + 1u;
  l->count++;
  return PL_OK;
}

bool pl_remove_at(PathList* l, int idx) {
  const char* p;
  uint32_t o, sz;
  if (l == NULL || (p = pl_get(l, idx)) == NULL) return false;
  o = (uint32_t)(p - l->pool);
  sz = (uint32_t)strlen(p) + 1u;
  memmove(l->pool + o, l->pool + o + sz, l->used - o - sz);
  l->used -= sz;
  l->count--;
  return true;
}

bool pl_remove(PathList* l, const char* path) {
  int i = pl_find(l, path);
  return i >= 0 && pl_remove_at(l, i);
}

int pl_parse(PathList* l, const char* text, uint32_t len) {
  uint32_t i = 0;
  char line[PL_PATH_MAX + 2];
  if (l == NULL) return 0;
  pl_clear(l);
  if (text == NULL) return 0;
  while (i < len) {
    uint32_t n = 0;
    int too_long = 0;
    while (i < len && text[i] != '\n') {
      if (n < PL_PATH_MAX + 1u) line[n++] = text[i]; else too_long = 1;
      i++;
    }
    i++;                                            /* skip the LF */
    if (n > 0 && line[n - 1] == '\r') n--;
    if (n == 0 || too_long || n > PL_PATH_MAX) continue;
    line[n] = 0;
    if (pl_add(l, line) == PL_FULL) break;
  }
  return l->count;
}

int pl_serialize(const PathList* l, char* out, uint32_t cap) {
  uint32_t i;
  if (l == NULL || out == NULL || l->used > cap) return -1;
  memcpy(out, l->pool, l->used);
  for (i = 0; i < l->used; i++) if (out[i] == 0) out[i] = '\n';
  return (int)l->used;
}
