// Stand-in for Doom's string helpers. Only the two the OPL player uses.

#ifndef JET_COMPAT_M_MISC_H
#define JET_COMPAT_M_MISC_H

#include <stddef.h>
#include "doomtype.h"

boolean M_snprintf(char *buf, size_t buf_len, const char *s, ...);
boolean M_StringConcat(char *dest, const char *src, size_t dest_size);

#endif
