/* Host tests for source/pathlist.c. */
#include <stdio.h>
#include <string.h>
#include "../source/pathlist.h"

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static char pool[64];
static char big[12288];

int main(void) {
  PathList l;
  char out[128];
  pl_init(&l, pool, sizeof pool, 0);
  CHECK(pl_count(&l) == 0 && pl_get(&l, 0) == NULL);
  CHECK(pl_add(&l, "/roms") == PL_OK);
  CHECK(pl_add(&l, "/saves/a.sav") == PL_OK);
  CHECK(pl_add(&l, "/roms") == PL_DUP && pl_count(&l) == 2);
  CHECK(pl_add(&l, "") == PL_BAD && pl_add(&l, "relative") == PL_BAD);
  CHECK(strcmp(pl_get(&l, 0), "/roms") == 0 && strcmp(pl_get(&l, 1), "/saves/a.sav") == 0);
  CHECK(pl_find(&l, "/saves/a.sav") == 1 && pl_find(&l, "/nope") == -1);
  CHECK(pl_add(&l, "/c") == PL_OK);
  /* remove middle keeps order */
  CHECK(pl_remove(&l, "/saves/a.sav") && pl_count(&l) == 2);
  CHECK(strcmp(pl_get(&l, 0), "/roms") == 0 && strcmp(pl_get(&l, 1), "/c") == 0);
  CHECK(!pl_remove(&l, "/saves/a.sav"));
  CHECK(!pl_remove_at(&l, 5) && !pl_remove_at(&l, -1));
  /* serialize / parse round trip */
  CHECK(pl_serialize(&l, out, sizeof out) == (int)l.used);
  CHECK(memcmp(out, "/roms\n/c\n", 9) == 0);
  CHECK(pl_serialize(&l, out, 3) == -1);
  {
    PathList m; char p2[64];
    pl_init(&m, p2, sizeof p2, 0);
    CHECK(pl_parse(&m, out, (uint32_t)pl_serialize(&l, out, sizeof out)) == 2);
    CHECK(strcmp(pl_get(&m, 1), "/c") == 0);
    /* CRLF, blanks, junk, duplicates, no trailing newline */
    { const char* t = "/a\r\n\r\n\nnot-abs\n/b\n/a\n/c"; CHECK(pl_parse(&m, t, (uint32_t)strlen(t)) == 3);
      CHECK(strcmp(pl_get(&m, 0), "/a") == 0 && strcmp(pl_get(&m, 2), "/c") == 0); }
    CHECK(pl_parse(&m, "", 0) == 0 && pl_count(&m) == 0);
  }
  /* pool full: pool bytes bound, not a count */
  pl_clear(&l);
  { int i, added = 0; char p[16];
    for (i = 0; i < 40; i++) { strcpy(p, "/x"); p[2] = (char)('a' + i % 26); p[3] = (char)('a' + i / 26); p[4] = 0;
      if (pl_add(&l, p) == PL_OK) added++; else break; }
    CHECK(added == 64 / 5 && l.used == 5u * (unsigned)added); CHECK(pl_add(&l, "/zzzz") == PL_FULL); }
  /* entry cap */
  { PathList s; char p3[256]; pl_init(&s, p3, sizeof p3, 2);
    CHECK(pl_add(&s, "/1") == PL_OK && pl_add(&s, "/2") == PL_OK && pl_add(&s, "/3") == PL_FULL);
    CHECK(pl_add(&s, "/1") == PL_DUP); }
  /* big pool, no count cap: 1000+ short paths */
  { PathList b; int i, ok = 0; char p[16]; pl_init(&b, big, sizeof big, 0);
    for (i = 0; i < 1500; i++) { sprintf(p, "/d%d", i); if (pl_add(&b, p) == PL_OK) ok++; }
    CHECK(ok == 1500); CHECK(strcmp(pl_get(&b, 1499), "/d1499") == 0);
    CHECK(pl_remove(&b, "/d0") && pl_count(&b) == 1499 && strcmp(pl_get(&b, 0), "/d1") == 0); }
  /* too-long path */
  { char lp[300]; memset(lp, 'a', 299); lp[0] = '/'; lp[299] = 0; pl_clear(&l); CHECK(pl_add(&l, lp) == PL_BAD); }
  printf("host_pathlist_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
