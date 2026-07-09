// T-Deck OPL backend — public interface for the Doom sound driver.
// The synth runs at the SFX mix rate; OPL_TDeck_Mix() ADDs music samples
// into the caller's 32-bit accumulation buffer (see i_tdeck_sound.c).

#ifndef OPL_TDECK_H
#define OPL_TDECK_H

#include <stdint.h>

// Sample rate shared by the SFX mixer and the OPL chip emulator.
#define OPL_TDECK_MIX_RATE     22050

// Largest number of samples the mixer requests per call; sizes the
// internal scratch buffer (OPL_TDeck_Mix chunks larger requests).
#define OPL_TDECK_MAX_SAMPLES  2048

void OPL_TDeck_Mix(int32_t *mix, int nsamples);

#endif /* OPL_TDECK_H */
