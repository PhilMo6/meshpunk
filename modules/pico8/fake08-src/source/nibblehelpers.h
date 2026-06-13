#pragma once

#include <string>
#include <cstdint>

//for 1 byte (8 bit) indexes, 128x64
//should be the equivalent of return y * 64 + (x / 2);
#define COMBINED_IDX(x, y) ((y) << 6) | ((x) >> 1)
#define IS_VALID_SPR_IDX(x, y) (y >= 0 && y < 128 && x >= 0 && x < 128)
//I think this should work if you cast the buffer to a uint32_t* pointer, but not tested
//for 4 byte (32 bit) inexes, 16x8
//should be the equivalent of return y * 16 + (x / 8);
//#define COMBINED_32_BIT_IDX(x, y) ((y) << 3) | ((x) >> 3)
//this may be hlpeful to optimize sprite blitting
//idea: get uint32_t from sprite buffer- should be 8 pixels
//split it up by bit shifting, and write to screen buffer as necessary

int getCombinedIdx(int x, int y);

int isValidSprIdx(int x, int y);

// Inline + bounds-checked: these are the final write/read gate for every
// per-pixel raster path. The bounds matter because draw routines clamp
// coordinates into drawState.clip_*, and carts can poke clip registers
// (0x5f20-0x5f23) past 128 — without this guard that walks pointer math
// up to ~8KB past the 8KB screen/sprite buffer (heap corruption).
// (1U << 0) & x picks odd pixels — the high nibble of the byte.
static inline void setPixelNibble(const int x, const int y, uint8_t value, uint8_t* targetBuffer) {
	if (!IS_VALID_SPR_IDX(x, y)) {
		return;
	}
	targetBuffer[COMBINED_IDX(x, y)] = ((1U << 0) & x)
		? (targetBuffer[COMBINED_IDX(x, y)] & 0x0f) | (value << 4 & 0xf0)
		: (targetBuffer[COMBINED_IDX(x, y)] & 0xf0) | (value & 0x0f);
}

static inline uint8_t getPixelNibble(const int x, const int y, const uint8_t* targetBuffer) {
	if (!IS_VALID_SPR_IDX(x, y)) {
		return 0;
	}
	return ((1U << 0) & x)
		? targetBuffer[COMBINED_IDX(x, y)] >> 4  //just last 4 bits
		: targetBuffer[COMBINED_IDX(x, y)] & 0x0f; //just first 4 bits
}
