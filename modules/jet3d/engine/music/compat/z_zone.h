// Stand-in for Doom's zone allocator.
//
// The zone exists so Doom can purge cached lumps under memory pressure. The
// music stack has no purgeable data -- the instrument bank is compiled in
// and a parsed song is freed explicitly -- so the tag is ignored and these
// are plain malloc/free. Only memio.c (behind the MUS converter) allocates
// at all.

#ifndef JET_COMPAT_Z_ZONE_H
#define JET_COMPAT_Z_ZONE_H

#include <stdlib.h>

#define PU_STATIC 1

#define Z_Malloc(size, tag, user)  malloc((size_t)(size))
#define Z_Free(ptr)                free(ptr)

#endif
