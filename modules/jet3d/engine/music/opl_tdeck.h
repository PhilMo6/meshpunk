// T-Deck OPL backend — public interface for the Doom sound driver.
// The synth runs at the SFX mix rate; OPL_TDeck_Mix() ADDs music samples
// into the caller's 32-bit accumulation buffer (see engine/jet_audio.cpp).

#ifndef OPL_TDECK_H
#define OPL_TDECK_H

#include <stdint.h>

// Sample rate shared by the SFX mixer and the OPL chip emulator.
// 22050 doubles the FM bandwidth (music harmonics to ~11 kHz) at 2x the
// mix/synth-resample cost. (An earlier drop to 11025 chased a starvation
// theory that turned out to be a signed/unsigned bug in the SFX
// interpolation — 22050 never got a fair trial before this retry.)
#define OPL_TDECK_MIX_RATE     22050

// Largest number of samples the mixer requests per call; sizes the
// internal scratch buffer (OPL_TDeck_Mix chunks larger requests).
#define OPL_TDECK_MAX_SAMPLES  2048

void OPL_TDeck_Mix(int32_t *mix, int nsamples);

#endif /* OPL_TDECK_H */
