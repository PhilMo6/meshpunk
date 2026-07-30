/*---------------------------------------------------------------------------
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version. See also the license.txt file for
 *	additional informations.
 *---------------------------------------------------------------------------
 */

#ifndef STDAFX_H
#define STDAFX_H

#include <stdint.h>

#ifdef _WIN32
#define path_default_slash() "\\"
#define path_default_slash_c() '\\'
#else
#define path_default_slash() "/"
#define path_default_slash_c() '/'
#endif

#endif /* STDAFX_H */
