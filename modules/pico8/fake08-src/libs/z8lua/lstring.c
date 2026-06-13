/*
** $Id: lstring.c,v 2.26 2013/01/08 13:50:10 roberto Exp $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/


#include <string.h>

#define lstring_c
#define LUA_CORE

#include "lua.h"

#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"

/* T-Deck debug trap: carts crash with deterministic nil-field reads that
** trace to duplicate interned short strings (same bytes, two TStrings).
** When a new short string is created, scan the whole table for an existing
** equal-bytes string; if one exists, interning just failed to find it —
** dump both strings' stored hash, bucket, and GC state to identify whether
** the stored hash was corrupted, the chain was broken by a resize, or the
** old string was wrongly swept. Remove once the root cause is fixed. */
/* Set 0 for release: the trap stayed armed through the whole hunt and never
** fired — the root cause was the OP_GETTABLE sandbox fallback in lvm.c. */
#define FAKE08_SPLIT_TRAP 0
#if FAKE08_SPLIT_TRAP
#include <stdio.h>
#include "lgc.h"
#endif


/*
** Lua will use at most ~(2^LUAI_HASHLIMIT) bytes from a string to
** compute its hash
*/
#if !defined(LUAI_HASHLIMIT)
#define LUAI_HASHLIMIT		5
#endif


/*
** equality for long strings
*/
int luaS_eqlngstr (TString *a, TString *b) {
  size_t len = a->tsv.len;
  lua_assert(a->tsv.tt == LUA_TLNGSTR && b->tsv.tt == LUA_TLNGSTR);
  return (a == b) ||  /* same instance or... */
    ((len == b->tsv.len) &&  /* equal length and ... */
     (memcmp(getstr(a), getstr(b), len) == 0));  /* equal contents */
}


/*
** equality for strings
*/
int luaS_eqstr (TString *a, TString *b) {
  return (a->tsv.tt == b->tsv.tt) &&
         (a->tsv.tt == LUA_TSHRSTR ? eqshrstr(a, b) : luaS_eqlngstr(a, b));
}


unsigned int luaS_hash (const char *str, size_t l, unsigned int seed) {
  unsigned int h = seed ^ cast(unsigned int, l);
  size_t l1;
  size_t step = (l >> LUAI_HASHLIMIT) + 1;
  for (l1 = l; l1 >= step; l1 -= step)
    h = h ^ ((h<<5) + (h>>2) + cast_byte(str[l1 - 1]));
  return h;
}


/*
** resizes the string table
*/
void luaS_resize (lua_State *L, int newsize) {
  int i;
  stringtable *tb = &G(L)->strt;
  /* cannot resize while GC is traversing strings */
  luaC_runtilstate(L, ~bitmask(GCSsweepstring));
  if (newsize > tb->size) {
    luaM_reallocvector(L, tb->hash, tb->size, newsize, GCObject *);
    for (i = tb->size; i < newsize; i++) tb->hash[i] = NULL;
  }
  /* rehash */
  for (i=0; i<tb->size; i++) {
    GCObject *p = tb->hash[i];
    tb->hash[i] = NULL;
    while (p) {  /* for each node in the list */
      GCObject *next = gch(p)->next;  /* save next */
      unsigned int h = lmod(gco2ts(p)->hash, newsize);  /* new position */
      gch(p)->next = tb->hash[h];  /* chain it */
      tb->hash[h] = p;
      resetoldbit(p);  /* see MOVE OLD rule */
      p = next;
    }
  }
  if (newsize < tb->size) {
    /* shrinking slice must be empty */
    lua_assert(tb->hash[newsize] == NULL && tb->hash[tb->size - 1] == NULL);
    luaM_reallocvector(L, tb->hash, tb->size, newsize, GCObject *);
  }
  tb->size = newsize;
}


/*
** creates a new string object
*/
static TString *createstrobj (lua_State *L, const char *str, size_t l,
                              int tag, unsigned int h, GCObject **list) {
  TString *ts;
  size_t totalsize;  /* total size of TString object */
  totalsize = sizeof(TString) + ((l + 1) * sizeof(char));
  ts = &luaC_newobj(L, tag, totalsize, list, 0)->ts;
  ts->tsv.len = l;
  ts->tsv.hash = h;
  ts->tsv.extra = 0;
  memcpy(ts+1, str, l*sizeof(char));
  ((char *)(ts+1))[l] = '\0';  /* ending 0 */
  return ts;
}


/*
** creates a new short string, inserting it into string table
*/
static TString *newshrstr (lua_State *L, const char *str, size_t l,
                                       unsigned int h) {
  GCObject **list;  /* (pointer to) list where it will be inserted */
  stringtable *tb = &G(L)->strt;
  TString *s;
  if (tb->nuse >= cast(lu_int32, tb->size) && tb->size <= MAX_INT/2)
    luaS_resize(L, tb->size*2);  /* too crowded */
  list = &tb->hash[lmod(h, tb->size)];
  s = createstrobj(L, str, l, LUA_TSHRSTR, h, list);
  tb->nuse++;
#if FAKE08_SPLIT_TRAP
  {
    /* a new short string was created: equal bytes must not already exist */
    int i;
    for (i = 0; i < tb->size; i++) {
      GCObject *o;
      for (o = tb->hash[i]; o != NULL; o = gch(o)->next) {
        TString *ts = rawgco2ts(o);
        if (ts != s && ts->tsv.tt == LUA_TSHRSTR && ts->tsv.len == l &&
            memcmp(getstr(ts), str, l) == 0) {
          printf("[SPLIT] '%.*s' old(p=%p stored_h=%08x bkt=%d expect_bkt=%d dead=%d) "
                 "new(p=%p h=%08x bkt=%d) size=%d nuse=%d\n",
                 (int)l, str,
                 (void *)ts, ts->tsv.hash, i, (int)lmod(ts->tsv.hash, tb->size),
                 isdead(G(L), o),
                 (void *)s, h, (int)lmod(h, tb->size),
                 tb->size, (int)tb->nuse);
        }
      }
    }
  }
#endif
  return s;
}


/*
** checks whether short string exists and reuses it or creates a new one
*/
static TString *internshrstr (lua_State *L, const char *str, size_t l) {
  GCObject *o;
  global_State *g = G(L);
  unsigned int h = luaS_hash(str, l, g->seed);
  for (o = g->strt.hash[lmod(h, g->strt.size)];
       o != NULL;
       o = gch(o)->next) {
    TString *ts = rawgco2ts(o);
    if (h == ts->tsv.hash &&
        l == ts->tsv.len &&
        (memcmp(str, getstr(ts), l * sizeof(char)) == 0)) {
      if (isdead(G(L), o))  /* string is dead (but was not collected yet)? */
        changewhite(o);  /* resurrect it */
      return ts;
    }
  }
  return newshrstr(L, str, l, h);  /* not found; create a new string */
}


/*
** new string (with explicit length)
*/
TString *luaS_newlstr (lua_State *L, const char *str, size_t l) {
  if (l <= LUAI_MAXSHORTLEN)  /* short string? */
    return internshrstr(L, str, l);
  else {
    if (l + 1 > (MAX_SIZET - sizeof(TString))/sizeof(char))
      luaM_toobig(L);
    return createstrobj(L, str, l, LUA_TLNGSTR, G(L)->seed, NULL);
  }
}


/*
** new zero-terminated string
*/
TString *luaS_new (lua_State *L, const char *str) {
  return luaS_newlstr(L, str, strlen(str));
}


Udata *luaS_newudata (lua_State *L, size_t s, Table *e) {
  Udata *u;
  if (s > MAX_SIZET - sizeof(Udata))
    luaM_toobig(L);
  u = &luaC_newobj(L, LUA_TUSERDATA, sizeof(Udata) + s, NULL, 0)->u;
  u->uv.len = s;
  u->uv.metatable = NULL;
  u->uv.env = e;
  return u;
}

