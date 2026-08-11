// Stand-in for Chocolate Doom's WAD interface.
//
// jet3d has no WAD. The OPL music player only ever asks for one lump --
// GENMIDI, its FM instrument bank -- so these three serve it from the bank
// compiled into the module and know nothing else. Keeping the names means
// i_oplmusic.c stays byte-identical to upstream, which matters for a GPL
// file we are carrying rather than authoring.

#ifndef JET_COMPAT_W_WAD_H
#define JET_COMPAT_W_WAD_H

#include "doomtype.h"

int   W_CheckNumForName(const char *name);
void *W_CacheLumpName(const char *name, int tag);
void  W_ReleaseLumpName(const char *name);

#endif
