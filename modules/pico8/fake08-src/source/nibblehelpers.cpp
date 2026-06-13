
#include <string>

#include "hostVmShared.h"
#include "nibblehelpers.h"


// setPixelNibble/getPixelNibble moved to nibblehelpers.h as bounds-checked
// inlines — they are the per-pixel hot path and the raster code's final
// bounds gate (see header comment).

int getCombinedIdx(const int x, const int y){
	//bit shifting might be faster? trying to optimize
    //return y * 64 + (x / 2);
	//return (y << 6) | (x >> 1);
	return COMBINED_IDX(x, y);
}

int isValidSprIdx(int x, int y){
	return IS_VALID_SPR_IDX(x, y);
}
